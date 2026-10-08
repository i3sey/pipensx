#!/usr/bin/env python3
"""Prepare offline description packs on a PC; never translates on the Switch.

Requires ctranslate2, sentencepiece, zstandard. --download-models explicitly
fetches Argos models; subsequent runs need no network. Progress/checkpoints
live outside resources. Inputs are the source catalog JSON, not credentials.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import time
import zipfile

LOCALES = {"en-US": "en", "pt-BR": "pt", "fr": "fr", "es": "es",
           "it": "it", "zh-CN": "zh", "ko": "ko", "ru": "ru"}


def source_records(paths):
    records = {}
    for path in paths:
        for entry in json.loads(Path(path).read_text(encoding="utf-8")):
            # Mirror CatalogService's byte limit, then strip its scraped prefix.
            raw = str(entry.get("description", "")).encode("utf-8")[:4096]
            if raw.startswith(b": "):
                raw = raw[2:]
            if raw:
                records[hashlib.sha1(raw).hexdigest()] = raw.decode("utf-8", "ignore")
    return records


def atomic_json(path, data):
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    temp.replace(path)


def prepare_models(root, download):
    pairs = [("ru", "en")] + [("en", x) for x in LOCALES.values() if x != "en"]
    models = {}
    index = None
    for src, dst in pairs:
        found = []
        for metadata in root.glob("*/metadata.json"):
            item = json.loads(metadata.read_text(encoding="utf-8"))
            if (item.get("from_code"), item.get("to_code")) == (src, dst):
                found.append(metadata.parent)
        if not found:
            if not download:
                raise RuntimeError(f"Missing offline model {src}->{dst}; use --download-models once")
            import requests
            if index is None:
                response = requests.get("https://raw.githubusercontent.com/argosopentech/argospm-index/main/index.json", timeout=60)
                response.raise_for_status()
                index = response.json()
            model = next(x for x in index if (x["from_code"], x["to_code"]) == (src, dst))
            archive = root / f"{src}_{dst}.argosmodel"
            if not archive.exists():
                print(f"Downloading model {src}->{dst}", flush=True)
                with requests.get(model["links"][0], stream=True, timeout=120) as response:
                    response.raise_for_status()
                    with archive.with_suffix(".tmp").open("wb") as output:
                        for chunk in response.iter_content(1 << 20):
                            output.write(chunk)
                archive.with_suffix(".tmp").replace(archive)
            with zipfile.ZipFile(archive) as archive_file:
                for info in archive_file.infolist():
                    target = (root / info.filename).resolve()
                    if not target.is_relative_to(root.resolve()):
                        raise RuntimeError("Unsafe model archive path")
                archive_file.extractall(root)
            return prepare_models(root, download)
        models[src, dst] = found[-1]
    return models


class Translator:
    def __init__(self, root, device, beam):
        import ctranslate2
        import sentencepiece
        self.processor = sentencepiece.SentencePieceProcessor(model_file=str(root / "sentencepiece.model"))
        self.model = ctranslate2.Translator(str(root / "model"), device=device,
                                           compute_type="int8_float16" if device == "cuda" else "int8",
                                           inter_threads=1, intra_threads=8)
        self.beam = beam

    def translate(self, texts):
        tokenized, owners, output = [], [], [[] for _ in texts]
        for owner, text in enumerate(texts):
            for sentence in re.split(r"(?<=[.!?。！？])\s+|\n+", text):
                if not sentence.strip():
                    continue
                tokens = self.processor.encode(sentence.strip(), out_type=str)
                # Bounded sentences avoid dropping long descriptions at model limits.
                for begin in range(0, len(tokens), 192):
                    tokenized.append(tokens[begin:begin + 192])
                    owners.append(owner)
        results = self.model.translate_batch(tokenized, beam_size=self.beam,
            max_batch_size=4096, batch_type="tokens", replace_unknowns=True,
            length_penalty=0.2, max_decoding_length=512)
        for owner, result in zip(owners, results):
            # Argos tokenizer also removes space markers left by copied
            # unknown pieces. Retain decode_pieces for Asian byte fallback.
            output[owner].append(self.processor.decode(result.hypotheses[0]).replace("▁", " "))
        return [" ".join(parts).strip() for parts in output]


def write_pack(path, descriptions):
    import zstandard
    keys = sorted(descriptions)
    records, blocks = [], []
    offset = 16 + len(keys) * 40
    compressor = zstandard.ZstdCompressor(level=12)
    for first in range(0, len(keys), 48):
        block_keys = keys[first:first + 48]
        plain = bytearray()
        ranges = []
        for key in block_keys:
            text = descriptions[key].encode("utf-8")
            if not text or len(text) > 65536:
                raise RuntimeError(f"Invalid translated text {key}")
            ranges.append((len(plain), len(text)))
            plain.extend(text)
        if len(plain) > 1048576:
            raise RuntimeError("Description block exceeds reader budget")
        compressed = compressor.compress(plain)
        for key, (begin, size) in zip(block_keys, ranges):
            records.append(bytes.fromhex(key) + struct.pack("<IIIII", offset, len(compressed), len(plain), begin, size))
        blocks.append(compressed)
        offset += len(compressed)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".tmp")
    with temp.open("wb") as output:
        output.write(b"PNXDESC1" + struct.pack("<II", 1, len(keys)))
        output.write(b"".join(records))
        for block in blocks:
            output.write(block)
    temp.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", nargs="+")
    parser.add_argument("--models", default=".codex/translation-models")
    parser.add_argument("--cache", default=".codex/description-translations")
    parser.add_argument("--output", default="resources/catalog/descriptions")
    parser.add_argument("--download-models", action="store_true")
    parser.add_argument("--device", choices=["cpu", "cuda"], default="cpu")
    parser.add_argument("--beam", type=int, default=2)
    parser.add_argument("--limit", type=int, default=0, help="Fixture/speed probe only; never marks full coverage")
    args = parser.parse_args()
    # Project venv CUDA wheels on Windows; retain directory handles for process life.
    handles = []
    if os.name == "nt":
        import sys
        cuda_paths = list(Path(sys.prefix, "Lib/site-packages/nvidia").glob("*/bin"))
        # CT2's native worker also resolves CUDA with LoadLibrary via PATH.
        os.environ["PATH"] = os.pathsep.join(str(p.resolve()) for p in cuda_paths) + os.pathsep + os.environ["PATH"]
        for path in cuda_paths:
            handles.append(os.add_dll_directory(str(path.resolve())))
    root, cache = Path(args.models), Path(args.cache)
    root.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    sources = source_records(args.catalog)
    if args.limit:
        sources = dict(list(sources.items())[:args.limit])
    models = prepare_models(root, args.download_models)
    print(f"Models ready; {len(sources)} source descriptions", flush=True)
    native_ru = {k: v for k, v in sources.items() if re.search(r"[А-Яа-яЁё]", v)}
    native_en = {k: v for k, v in sources.items() if k not in native_ru}
    stats = {}
    corrections_path = Path("resources/catalog/description_overrides.json")
    corrections = json.loads(corrections_path.read_text(encoding="utf-8")) if corrections_path.exists() else {}
    for locale, entries in corrections.items():
        if locale not in LOCALES or not isinstance(entries, dict):
            raise ValueError("Unsupported offline correction locale")
        for key, text in entries.items():
            if not re.fullmatch(r"[0-9a-f]{40}", key) or not isinstance(text, str) or not text.strip() or len(text.encode("utf-8")) > 65536:
                raise ValueError("Invalid offline description correction")
    for locale, target in LOCALES.items():
        file = cache / f"{locale}.json"
        translated = json.loads(file.read_text(encoding="utf-8")) if file.exists() else {}
        translated = {key: value.replace("▁", " ") for key, value in translated.items()}
        translated.update(native_ru if target == "ru" else native_en if target == "en" else {})
        input_texts = sources if target == "en" else json.loads((cache / "en-US.json").read_text(encoding="utf-8"))
        pending = [key for key in sources if key not in translated]
        print(f"Loading {locale} model; {len(pending)} descriptions pending", flush=True)
        worker = Translator(models["ru" if target == "en" else "en", target], args.device, args.beam) if pending else None
        started = time.monotonic()
        for first in range(0, len(pending), 32):
            keys = pending[first:first + 32]
            values = worker.translate([input_texts[key] for key in keys])
            for key, value in zip(keys, values):
                if not value:
                    raise RuntimeError(f"Empty translation {locale} {key}")
                translated[key] = value
            atomic_json(file, translated)
            if first % 160 == 0:
                print(f"{locale}: {min(first + 32, len(pending))}/{len(pending)} pending, elapsed={time.monotonic()-started:.1f}s", flush=True)
        # Write only this source snapshot, so old cache entries do not bloat the NRO.
        complete = {key: translated[key] for key in sources}
        reviewed = {key: value for key, value in corrections.get(locale, {}).items() if key in sources}
        complete.update(reviewed)
        write_pack(Path(args.output) / f"{locale}.pdx", complete)
        native_count = len(native_ru) if target == "ru" else len(native_en) if target == "en" else 0
        stats[locale] = {"entries": len(complete), "seconds": round(time.monotonic()-started, 2),
                         "native_entries": native_count, "translated_entries": len(complete) - native_count,
                         "reviewed_entries": len(reviewed),
                         "machine_translated": len(complete) > native_count}
        print(f"Completed {locale}: {len(complete)} descriptions", flush=True)
        if worker:
            del worker
    atomic_json(Path(args.output) / "coverage.json", {"schema": 1, "fixture": bool(args.limit), "source_count": len(sources), "languages": stats})


if __name__ == "__main__":
    main()
