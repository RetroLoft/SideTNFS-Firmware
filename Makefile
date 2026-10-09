# Version from file
VERSION := $(shell cat version.txt)

## Tag this version
.PHONY: tag
tag:
	git tag $(VERSION) && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"

.PHONY: native test-native falcon test-falcon
native:
	python3 tools/build_native.py
test-native:
	python3 tools/test_native.py

# Compatibility aliases for existing build commands.
falcon: native
test-falcon: test-native
