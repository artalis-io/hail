# Static Hail landing page, following Keel's dependency-free site pattern.
.PHONY: all build check clean serve
BUILD_DIR := build
STATIC_FILES := index.html hail.css mark.svg og-card.svg
all: build
build:
	@mkdir -p "$(BUILD_DIR)"
	@cp $(STATIC_FILES) "$(BUILD_DIR)/"
	@echo "Site built in $(BUILD_DIR)/"
check:
	@MAKE="$(MAKE)" perl check_site.pl
serve: build
	@cd "$(BUILD_DIR)" && python3 -m http.server 8000
clean:
	rm -rf "$(BUILD_DIR)"
