.PHONY: clean All

All:
	@echo "----------Building project:[ vclient - Debug ]----------"
	@cd "vclient" && "$(MAKE)" -f  "vclient.mk"
clean:
	@echo "----------Cleaning project:[ vclient - Debug ]----------"
	@cd "vclient" && "$(MAKE)" -f  "vclient.mk" clean
