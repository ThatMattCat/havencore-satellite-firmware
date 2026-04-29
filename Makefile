# Convenience wrapper for the dev-loop OTA push path. ESP-IDF builds are
# still done via `idf.py build` (which manages its own dependencies); this
# Makefile only knows how to ship a built binary to a running device.
#
#   make ota IP=10.0.0.42         # push build/havencore_satellite.bin
#   make ota IP=satellite-living  # mDNS / DNS name also fine
#   make version IP=10.0.0.42     # query running device
#
# The device must already be running a firmware that includes the dev
# OTA HTTP server (havencore_ota_dev_server_start()). First-flash still
# requires `idf.py -p <port> flash`.

BIN ?= build/havencore_satellite.bin
PORT ?= /dev/ttyACM0
CURL ?= curl

.PHONY: ota version flash publish help

help:
	@echo "make ota IP=<addr>      Push $(BIN) to http://<addr>/dev/ota"
	@echo "make version IP=<addr>  GET http://<addr>/dev/version"
	@echo "make flash              idf.py -p $(PORT) flash monitor"
	@echo "make publish            scp $(BIN) to the HavenCore web server"
	@echo "                        (configured via .publish.env; auto-runs on idf.py build)"
	@echo ""
	@echo "Variables: BIN=$(BIN) PORT=$(PORT)"

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
