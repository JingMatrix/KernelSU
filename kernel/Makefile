KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ODIR := $(MDIR)
KMI := $(notdir $(patsubst %/,%,$(KDIR)))

ifeq ($(KMI),android17-6.18)
KBUILD_ARGS := M=$(MDIR) MO=$(ODIR)
else
KBUILD_ARGS := M=$(ODIR) src=$(MDIR)
endif

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))
$(info -- OUTPUT DIR: $(ODIR))
$(info -- KMI: $(KMI))
$(info -- KBUILD_ARGS: $(KBUILD_ARGS))

# compile_commands.json as a native kbuild target (scripts/clang-tools'
# integration) postdates this project's own minimum supported kernel --
# request it only when the target tree's own top-level Makefile actually
# defines the rule, so an older kernel's build does not fail entirely over a
# missing IDE convenience this Makefile's own compdb: target
# (generate_compdb.py, below) already covers independently.
KSU_HAS_COMPILE_COMMANDS := $(shell grep -q "^compile_commands.json:" $(KDIR)/../Makefile 2>/dev/null && echo 1)

.PHONY: all compdb clean format check-format

all: check_symbol
	$(MAKE) -C $(KDIR) $(KBUILD_ARGS) modules $(if $(KSU_HAS_COMPILE_COMMANDS),compile_commands.json) -j$(shell nproc)
	./check_symbol $(ODIR)/kernelsu.ko $(KDIR)/vmlinux

compdb:
	python3 $(MDIR)/.vscode/generate_compdb.py -O $(ODIR) $(MDIR)
clean:
	$(MAKE) -C $(KDIR) $(KBUILD_ARGS) clean
	rm check_symbol
check_symbol: tools/check_symbol.c
	$(CC) tools/check_symbol.c -o check_symbol
format:
	find . \( -name "*.c" -o -name "*.h" \) -print0 | xargs -0 clang-format -i
check-format:
	find . \( -name "*.c" -o -name "*.h" \) -print0 | xargs -0 clang-format --dry-run --Werror

# Keep a new line here!! Because someone may append config
