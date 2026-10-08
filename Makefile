# Version from file
VERSION := $(shell cat version.txt)

## Tag this version
.PHONY: tag
tag:
	git tag $(VERSION) && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"

.PHONY: falcon test-falcon
falcon:
	python3 tools/build_falcon.py
test-falcon:
	python3 tools/test_native.py
