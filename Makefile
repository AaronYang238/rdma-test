# 顶层 Makefile —— 递归构建所有 examples/。
#
# 旧的单体 demo 已收编为 examples/01-write-demo/；保留 src/ 仅作历史参照。

EXAMPLES := $(sort $(dir $(wildcard examples/*/Makefile)))

.PHONY: all clean list $(EXAMPLES)

all: $(EXAMPLES)

$(EXAMPLES):
	$(MAKE) -C $@

clean:
	@for d in $(EXAMPLES); do $(MAKE) -C $$d clean; done

list:
	@echo "可构建示例："
	@for d in $(EXAMPLES); do echo "  - $$d"; done
