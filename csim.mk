SKETCH_NAME=$(shell basename `ls -1tr *.ino | tail -1 | sed s/.ino//`)
CSIM_BUILD_DIR=./build/csim
CSIM_LIBS+=esp32csim
CCACHE=ccache
CSIM_SRC_DIRS+=$(foreach L,$(CSIM_LIBS),${HOME}/Arduino/libraries/${L}/src)
CSIM_SRC_DIRS+=$(foreach L,$(CSIM_LIBS),${HOME}/Arduino/libraries/${L})
CSIM_SRC_DIRS+=$(foreach L,$(CSIM_LIBS),${HOME}/Arduino/libraries/${L}/src/csim_include)
CSIM_SRC_CPP+=$(foreach DIR,$(CSIM_SRC_DIRS),$(wildcard $(DIR)/*.cpp)) 
CSIM_SRC_C+=$(foreach DIR,$(CSIM_SRC_DIRS),$(wildcard $(DIR)/*.c)) 
CSIM_SRC_C_WITHOUT_PATH = $(notdir $(CSIM_SRC_C))
CSIM_SRC_CPP_WITHOUT_PATH = $(notdir $(CSIM_SRC_CPP))
CSIM_OBJS_C=$(CSIM_SRC_C_WITHOUT_PATH:%.c=${CSIM_BUILD_DIR}/%.o)
CSIM_OBJS_CPP=$(CSIM_SRC_CPP_WITHOUT_PATH:%.cpp=${CSIM_BUILD_DIR}/%.o)
CSIM_OBJS=$(CSIM_OBJS_CPP) $(CSIM_OBJS_C)
CSIM_INC+=$(foreach DIR,$(CSIM_SRC_DIRS),-I${DIR})

CSIM_CFLAGS+=-g -ffunction-sections -Wl,--gc-sections -MMD -DGIT_VERSION=\"${GIT_VERSION}\" -DESP32 -DCSIM -DUBUNTU 
CSIM_CPPFLAGS=$(CSIM_CFLAGS) -fpermissive
#CSIM_CFLAGS+=-DGPROF=1 -pg
#CSIM_CFLAGS+=-O2

${CSIM_BUILD_DIR}/%.o: %.c 
	echo $@
	${CCACHE} gcc ${CSIM_CFLAGS} -c ${CSIM_INC} $< -o $@

${CSIM_BUILD_DIR}/%.o: %.cpp 
	echo $@
	${CCACHE} g++ ${CSIM_CPPFLAGS} -c ${CSIM_INC} $< -o $@

${CSIM_BUILD_DIR}/%.o: %.ino
	echo $@
	${CCACHE} g++ ${CSIM_CPPFLAGS} -x c++ -c ${CSIM_INC} $< -o $@

${SKETCH_NAME}_csim: ${CSIM_BUILD_DIR} ${CSIM_OBJS} ${CSIM_BUILD_DIR}/${SKETCH_NAME}.o
	echo $@
	g++ -g ${CSIM_CFLAGS} ${CSIM_OBJS} ${CSIM_BUILD_DIR}/${SKETCH_NAME}.o -o $@         

csim: ${SKETCH_NAME}_csim 
	cp $< $@

${CSIM_BUILD_DIR}:
	mkdir -p ${CSIM_BUILD_DIR}

VPATH = $(sort $(dir $(CSIM_SRC_C)) $(dir $(CSIM_SRC_CPP)))

.PHONY: clean
clean:
	rm -f ${CSIM_BUILD_DIR}/*.[od] ${SKETCH_NAME}_csim csim

-include ${CSIM_BUILD_DIR}/*.d


