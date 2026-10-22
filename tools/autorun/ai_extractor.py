"""
AI & Heuristic Extractor for AutoRun PC game compatibility reports.
Extracts structured game profiles from Telegram comments, Discord messages, and forum posts.
"""

from __future__ import annotations
import json
import re
import urllib.request
import urllib.error
from typing import Dict, Any, Optional


def extract_with_regex(text: str, mid: int = 0, author: str = "", date: str = "") -> Optional[Dict[str, Any]]:
    """Extract fields using regex pattern matching on the 1-6 template."""
    clean_text = text.replace("**", "").strip()
    if len(clean_text) < 15:
        return None

    title = ""
    graphics = ""
    fps = ""
    oc = ""
    lsfg = ""
    bugs = ""

    # 1. Title
    m_t = re.search(r'(?:1\.\s*(?:Название(?:\s+игры)?:?)?)(.*?)(?=\n\s*2\.|\n\s*Графика|$)', clean_text, re.IGNORECASE)
    if m_t:
        title = m_t.group(1).strip()
    elif clean_text.startswith("🟢") or clean_text.startswith("🟠") or clean_text.startswith("🔴"):
        title = clean_text.split('\n')[0].replace('🟢', '').replace('🟠', '').replace('🔴', '').split('(')[0].strip()

    if not title:
        first_line = clean_text.split('\n')[0].strip()
        if len(first_line) < 60 and not first_line.startswith("http"):
            title = first_line

    if not title or len(title) < 2:
        return None

    # 2. Graphics
    m_g = re.search(r'(?:2\.\s*(?:Графика.*?:?)?)(.*?)(?=\n\s*3\.|\n\s*Сколько FPS|$)', clean_text, re.IGNORECASE)
    if m_g:
        graphics = m_g.group(1).strip()

    # 3. FPS
    m_f = re.search(r'(?:3\.\s*(?:Сколько FPS.*?:?)?)(.*?)(?=\n\s*4\.|\n\s*Разгон|$)', clean_text, re.IGNORECASE)
    if m_f:
        fps = m_f.group(1).strip()

    # 4. OC
    m_o = re.search(r'(?:4\.\s*(?:Разгон.*?:?)?)(.*?)(?=\n\s*5\.|\n\s*Включен.*?LSFG|$)', clean_text, re.IGNORECASE)
    if m_o:
        oc = m_o.group(1).strip()

    # 5. LSFG
    m_l = re.search(r'(?:5\.\s*(?:Включен.*?LSFG.*?:?)?)(.*?)(?=\n\s*6\.|\n\s*Баги|$)', clean_text, re.IGNORECASE)
    if m_l:
        lsfg = m_l.group(1).strip()

    # 6. Bugs
    m_b = re.search(r'(?:6\.\s*(?:Баги.*?:?)?)(.*)', clean_text, re.IGNORECASE | re.DOTALL)
    if m_b:
        bugs = m_b.group(1).strip()
    else:
        bugs = clean_text

    # Status detection
    lower = (title + " " + fps + " " + bugs + " " + clean_text).lower()
    if any(k in lower for k in ["неиграбельно", "слайдшоу", "слайд-шоу", "8-15", "5-15", "не работает", "вылетает"]):
        status = "Неиграбельно / Проблемы"
        badge = "🔴"
    elif any(k in lower for k in ["нюанс", "просадки", "13-30", "20-30", "25-30", "фикс", "твик", "настройки"]):
        status = "Играбельно с нюансами"
        badge = "🟡"
    else:
        status = "Играбельно"
        badge = "🟢"

    # Translator
    translator = "Авто (Box64/FEX)"
    if "fex" in lower:
        translator = "FEX"
    elif "box64" in lower:
        translator = "Box64"

    # Renderer
    renderer = "DXVK"
    if "wined3d" in lower:
        renderer = "WineD3D"
    elif "vkd3d" in lower:
        renderer = "DXVK + VKD3D"

    clean_dir = re.sub(r'[^a-zA-Z0-9_-]', '_', title)

    return {
        "game_title": title,
        "status": status,
        "status_badge": badge,
        "fps": fps or "Зависит от настроек",
        "graphics_resolution": graphics or "720p / по умолчанию",
        "renderer_and_tools": renderer,
        "cpu_translator": translator,
        "overclock": oc or "Не указан",
        "lsfg": lsfg or "Нет",
        "repack_version": "FitGirl Repack" if "fitgirl" in lower else ("4pda сборка" if "4pda" in lower else "Любой / Portable"),
        "notes_and_fixes": bugs or "Без замечаний",
        "registry_tweaks": "user.reg твик" if ("user.reg" in lower or "реестр" in lower) else "",
        "autorun_dir": f"switch/wine/drive_c/Games/{clean_dir}",
        "source": f"Telegram @AutoRunn/275 (сообщение #{mid})" if mid else "Telegram @AutoRunn/275",
        "author": author,
        "date": date
    }


def extract_with_local_ai(text: str, api_url: str = "http://localhost:1234/v1/chat/completions", model: Optional[str] = None) -> Optional[Dict[str, Any]]:
    """
    Extract structured fields using a local LLM (LM Studio / Ollama / OpenAI-compatible).
    Falls back to extract_with_regex if local server is unreachable.
    """
    prompt = f"""Ты — аналитик данных по эмуляции ПК-игр на Nintendo Switch (эмулятор AutoRun / Wine + Box64 / FEX).
Проанализируй текст сообщения игрока и верни JSON со следующими полями:
- game_title: точное название игры
- status: "Играбельно" | "Играбельно с нюансами" | "Неиграбельно / Проблемы"
- status_badge: "🟢" | "🟡" | "🔴"
- fps: показатели FPS (например "30 FPS стабильно" или "10-15 кадров")
- graphics_resolution: разрешение и настройки графики
- renderer_and_tools: DXVK / WineD3D / VKD3D
- cpu_translator: FEX или Box64
- overclock: частоты разгона CPU / GPU / RAM
- lsfg: "Да" или "Нет"
- repack_version: рекомендуемый репак (FitGirl, 4pda, etc) или "Любой"
- notes_and_fixes: краткое описание багов, необходимых фиксов или твиков
- registry_tweaks: нужны ли твики user.reg (кратко что именно)

Текст сообщения:
\"\"\"{text}\"\"\"

Ответь ТОЛЬКО валидным JSON без лишних пояснений."""

    payload = {
        "messages": [
            {"role": "system", "content": "You are a specialized game compatibility parser. Return only JSON."},
            {"role": "user", "content": prompt}
        ],
        "temperature": 0.1,
    }
    if model:
        payload["model"] = model

    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        api_url,
        data=data,
        headers={"Content-Type": "application/json"}
    )

    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            res_data = json.loads(resp.read().decode("utf-8"))
            content = res_data["choices"][0]["message"]["content"]
            # Clean markdown codeblocks if any
            content = re.sub(r'^```(?:json)?\s*', '', content.strip())
            content = re.sub(r'\s*```$', '', content.strip())
            parsed = json.loads(content)
            title = parsed.get("game_title", "")
            clean_dir = re.sub(r'[^a-zA-Z0-9_-]', '_', title)
            parsed["autorun_dir"] = f"switch/wine/drive_c/Games/{clean_dir}"
            return parsed
    except Exception as e:
        # Fallback to regex
        return extract_with_regex(text)
