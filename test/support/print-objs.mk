# Extra makefile (-f) that exposes the object lists of the repo's own Makefile, so test
# tooling builds exactly what the sim target builds instead of keeping a copy of the list.
.PHONY: print-lib-objs print-app-objs
print-lib-objs:
	@echo $(LIB_OBJS)
print-app-objs:
	@echo $(APP_OBJS)
