.PHONY: abi-compat-test architecture-test benchmark bootstrap commit-message-test contract-test dashboard dashboard-test dependency-test diff-check doctor flutter-test format harness-v2-test macos-bundle-test native-test project-model-test security-test vcpkg-bootstrap verify

abi-compat-test:
	@tool/harness/abi_compat_test.sh

architecture-test:
	@tool/harness/architecture_test.sh

benchmark:
	@tool/harness/performance_test.sh

dependency-test:
	@tool/harness/dependency_test.sh

diff-check:
	@tool/harness/diff_check.sh

doctor:
	@tool/harness/doctor.sh

bootstrap:
	@tool/harness/bootstrap.sh

commit-message-test:
	@tool/harness/commit_message_test.sh

contract-test:
	@python3 -B tool/harness/agent.py --local validate

dashboard:
	@python3 -B tool/harness/dashboard.py

dashboard-test:
	@python3 -B -m unittest discover -s tool/harness/tests -p 'test_dashboard.py'

native-test:
	@tool/harness/native_test.sh

security-test:
	@tool/harness/security_test.sh

vcpkg-bootstrap:
	@tool/harness/vcpkg_bootstrap.sh

flutter-test:
	@tool/harness/flutter_test.sh

harness-v2-test:
	@python3 -B -m unittest discover -s tool/harness/tests -p 'test_*.py'

project-model-test:
	@tool/harness/project_model_test.sh

macos-bundle-test:
	@tool/harness/macos_bundle_test.sh

verify:
	@tool/harness/verify.sh

format:
	@tool/harness/format.sh
