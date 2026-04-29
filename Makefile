# Convenience wrapper for the dev-loop OTA push path. ESP-IDF builds are
# still done via `idf.py build` (which manages its own dependencies); this
# Makefile only knows how to bump the version, ship a built binary to a
# running device, and chain those for a one-shot deploy.
#
#   make build                    # bump version.txt + idf.py build
#   make ota IP=10.0.0.42         # push build/havencore_satellite.bin
#   make ota IP=satellite-living  # mDNS / DNS name also fine
#   make deploy IP=10.0.0.42      # build + ota in one shot
#   make version IP=10.0.0.42     # query running device
#
# The device must already be running a firmware that includes the dev
# OTA HTTP server (havencore_ota_dev_server_start()). First-flash still
# requires `idf.py -p <port> flash`.
#
# Why `make build` instead of `idf.py build`: scripts/bump_version.sh
# stamps version.txt with a fresh timestamp on every dev build, so the
# Settings -> Update Firmware pull path actually pulls (otherwise both
# ends compute the same `-dirty` string and the device says "Up to
# date"). `idf.py build` directly still works but skips the bump.

BIN ?= build/havencore_satellite.bin
PORT ?= /dev/ttyACM0
CURL ?= curl

.PHONY: build ota deploy version flash publish help

help:
	@echo "make build              Bump version.txt + idf.py build"
	@echo "make ota IP=<addr>      Push $(BIN) to http://<addr>/dev/ota"
	@echo "make deploy IP=<addr>   build + ota in one shot"
	@echo "make version IP=<addr>  GET http://<addr>/dev/version"
	@echo "make flash              idf.py -p $(PORT) flash monitor"
	@echo "make publish            scp $(BIN) to the HavenCore web server"
	@echo "                        (configured via .publish.env; auto-runs on idf.py build)"
	@echo ""
	@echo "Variables: BIN=$(BIN) PORT=$(PORT)"

build:
	@bash scripts/bump_version.sh
	idf.py build

deploy: build
	@$(MAKE) ota IP=$(IP)

ota:
	@if [ -z "$(IP)" ]; then echo "error: IP=<addr> required" >&2; exit 2; fi
	@if [ ! -f "$(BIN)" ]; then echo "error: $(BIN) not found - run idf.py build first" >&2; exit 1; fi
	@echo "==> pushing $(BIN) ($$(stat -c%s $(BIN)) bytes) to http://$(IP)/dev/ota"
	$(CURL) --fail --max-time 120 \
		-H "Content-Type: application/octet-stream" \
		--data-binary "@$(BIN)" \
		"http://$(IP)/dev/ota"
	@echo

version:
	@if [ -z "$(IP)" ]; then echo "error: IP=<addr> required" >&2; exit 2; fi
	$(CURL) --fail --max-time 5 "http://$(IP)/dev/version"
	@echo

flash:
	idf.py -p $(PORT) flash monitor

publish:
	@if [ ! -f "$(BIN)" ]; then echo "error: $(BIN) not found - run idf.py build first" >&2; exit 1; fi
	bash scripts/publish_firmware.sh "$(BIN)"
