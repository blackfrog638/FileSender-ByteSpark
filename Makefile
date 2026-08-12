.PHONY: abi-compat-test architecture-test benchmark bootstrap commit-message-test dashboard-test delivery-plan-test dependency-test doctor evidence-test flutter-test format governance-test harness-v2-test macos-bundle-test native-test security-test tdd-proof-test vcpkg-bootstrap verify

abi-compat-test:
	@tool/harness/abi_compat_test.sh

architecture-test:
	@tool/harness/architecture_test.sh

benchmark:
	@tool/harness/performance_test.sh

dependency-test:
	@tool/harness/dependency_test.sh

delivery-plan-test:
	@python3 -B tool/harness/delivery_plan_test.py

doctor:
	@tool/harness/doctor.sh

evidence-test:
	@python3 -B tool/harness/evidence_test.py

bootstrap:
	@tool/harness/bootstrap.sh

commit-message-test:
	@tool/harness/commit_message_test.sh

dashboard-test:
	@python3 -B tool/harness/dashboard_test.py

native-test:
	@tool/harness/native_test.sh

security-test:
	@tool/harness/security_test.sh

vcpkg-bootstrap:
	@tool/harness/vcpkg_bootstrap.sh

flutter-test:
	@tool/harness/flutter_test.sh

governance-test:
	@tool/harness/governance_test.sh

harness-v2-test:
	@python3 -B -m unittest discover -s tool/harness/tests -p 'test_*.py'

tdd-proof-test:
	@python3 -B tool/harness/tdd_proof_test.py

macos-bundle-test:
	@tool/harness/macos_bundle_test.sh

verify:
	@tool/harness/verify.sh

format:
	@tool/harness/format.sh
