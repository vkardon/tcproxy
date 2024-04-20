
# Executable to build
EXE = tcproxy 

# Sources
PROJECT_HOME = .
OBJ_DIR = $(PROJECT_HOME)/_obj

SRCS = $(PROJECT_HOME)/main.cpp \
       $(PROJECT_HOME)/config.cpp \
       $(PROJECT_HOME)/tcproxy.cpp

# Include directories
INCS = -I$(PROJECT_HOME)

# Libraries
LIBS = 

# Objective files to build
OBJS = $(addprefix $(OBJ_DIR)/, $(addsuffix .o, $(basename $(notdir $(SRCS)))))

# Get information about current kernel to distinguish between RedHat6 vs. Redhat7
OS = $(shell uname -s)

# Compiler and linker to use
ifeq "$(OS)" "Linux"
  CC = g++
else
  CC = g++
endif

CFLAGS = -std=c++11 -Wall -pthread
#CFLAGS = -std=c++11 -Wall -pthread -O3 -s -DNDEBUG
LD = $(CC)
LDFLAGS = -pthread

# Build executable
$(EXE): $(OBJS)
	$(LD) $(LDFLAGS) -o $(EXE) $(OBJS) $(LIBS)

# Compile source files
# Add -MP to generate dependency list
# Add -MMD to not include system headers
$(OBJ_DIR)/%.o: $(PROJECT_HOME)/%.cpp Makefile
	-mkdir -p $(OBJ_DIR)
	$(CC) -c -MP -MMD $(CFLAGS) $(INCS) -o $(OBJ_DIR)/$*.o $<
	
# Delete all intermediate files
clean: 
#	@echo OBJS = $(OBJS)
	rm -rf $(EXE) $(OBJ_DIR) core

#
# Read the dependency files.
# Note: use '-' prefix to don't display error or warning
# if include file do not exist (just remade it)
#
-include $(OBJS:.o=.d)

