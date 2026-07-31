.DEFAULT_GOAL := help

CMAKE_BIN ?= cmake
PIPENSX_METADATA_INDEX ?= $(CURDIR)/resources/catalog/game_metadata_index.json
MTP_DIR ?=
NRO_SRC ?= $(CURDIR)/build-switch/pipensx.nro
DEPLOY_CLEAN ?= 0

.PHONY: help pc test switch probe golden clean audit deploy

help:
	@echo "pipensx build targets:"
	@echo "  make pc       Build the portable command-line client"
	@echo "  make test     Build and run the PC test suite"
	@echo "  make switch   Build build-switch/pipensx.nro"
	@echo "  make probe    Build the sysmodule probe (src/probe/README.md)"
	@echo "  make golden   Run deterministic UI screenshot tests"
	@echo "  make audit    Scan the complete Git history with gitleaks"
	@echo "  make deploy MTP_DIR='mtp://...' [DEPLOY_CLEAN=1]"
	@echo "  make clean    Remove PC, Switch, and golden build outputs"

pc:
	$(MAKE) -f Makefile.pc

test:
	$(MAKE) -f Makefile.pc test

switch:
	CMAKE_BIN="$(CMAKE_BIN)" \
	PIPENSX_METADATA_INDEX="$(PIPENSX_METADATA_INDEX)" \
	$(MAKE) -f Makefile.switch

# Kept out of `make switch`: the probe is an investigation artifact, not part of
# the app's release path.
probe: switch
	$(CMAKE_BIN) --build build-switch --target pipensx_probe_nsp --parallel
	$(CMAKE_BIN) --build build-switch --target pipensx_probe_client_nro --parallel
	@echo "Probe SD layout: $(CURDIR)/build-switch/probe/"

golden:
	CMAKE_BIN="$(CMAKE_BIN)" scripts/golden.sh check

audit:
	@command -v gitleaks >/dev/null || { \
		echo "gitleaks is required: https://github.com/gitleaks/gitleaks" >&2; \
		exit 2; \
	}
	gitleaks git . --redact --no-banner

deploy:
	MTP_DIR="$(MTP_DIR)" NRO_SRC="$(NRO_SRC)" \
	DEPLOY_CLEAN="$(DEPLOY_CLEAN)" scripts/deploy_switch.sh

clean:
	$(MAKE) -f Makefile.pc clean
	$(MAKE) -f Makefile.switch clean
	rm -rf build-golden
