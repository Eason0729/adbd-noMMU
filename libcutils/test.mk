include Makefile
$(info OBJS = '$(OBJS)')
$(info CXX_SRC = '$(LOCAL_CXX_SRC_FILES)')
test: $(OBJS)
	@echo "OBJS = $(OBJS)"
