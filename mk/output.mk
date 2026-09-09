# Build output formatting: colors, phase markers, status icons.
#
# Each macro expands to a self-contained shell command that detects a real
# terminal at execution time (`[ -t 1 ]`). Colors are emitted only on a tty
# and disabled when NO_COLOR is set (https://no-color.org), so piping the
# build log (`make | tee`) or CI logs stay clean.
#
# Usage in a recipe:
#   @$(call phase,Userspace)
#   @$(call ok,built)
#   @$(call warn,deprecated)
#   @$(call fail,broken)
#   @$(call step,    LD build/kernel.elf)

phase = if [ -t 1 ] && [ -z "$$NO_COLOR" ]; then \
	printf '\033[1;36m>>> %s\033[0m\n' '$(1)'; \
	else printf '>>> %s\n' '$(1)'; fi

ok = if [ -t 1 ] && [ -z "$$NO_COLOR" ]; then \
	printf '\033[1;32m  [OK] %s\033[0m\n' '$(1)'; \
	else printf '  [OK] %s\n' '$(1)'; fi

warn = if [ -t 1 ] && [ -z "$$NO_COLOR" ]; then \
	printf '\033[1;33m  [WARN] %s\033[0m\n' '$(1)'; \
	else printf '  [WARN] %s\n' '$(1)'; fi

fail = if [ -t 1 ] && [ -z "$$NO_COLOR" ]; then \
	printf '\033[1;31m  [FAIL] %s\033[0m\n' '$(1)'; \
	else printf '  [FAIL] %s\n' '$(1)'; fi

step = if [ -t 1 ] && [ -z "$$NO_COLOR" ]; then \
	printf '\033[2m  %s\033[0m\n' '$(1)'; \
	else printf '  %s\n' '$(1)'; fi

divider = if [ -t 1 ] && [ -z "$$NO_COLOR" ]; then \
	printf '\033[2m========================================\033[0m\n'; \
	else printf '========================================\n'; fi