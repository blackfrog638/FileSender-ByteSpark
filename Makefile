.PHONY: benchmark bootstrap doctor flutter-test format macos-bundle-test native-test security-test verify

benchmark:
	@tool/harness/performance_test.sh

doctor:
	@tool/harness/doctor.sh

bootstrap:
	@tool/harness/bootstrap.sh

native-test:
	@tool/harness/native_test.sh

security-test:
	@tool/harness/security_test.sh

flutter-test:
	@tool/harness/flutter_test.sh

macos-bundle-test:
	@tool/harness/macos_bundle_test.sh

verify:
	@tool/harness/verify.sh

format:
	@tool/harness/format.sh
