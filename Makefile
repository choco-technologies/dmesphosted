# #############################################################################
# 
# 	dmesphosted - Wi-Fi on an ESP-Hosted co-processor, over SPI.
#
# #############################################################################
DMOD_DIR=@DMOD_DIR@

# -----------------------------------------------------------------------------
#  Paths initialization
# -----------------------------------------------------------------------------
include $(DMOD_DIR)/paths.mk

# -----------------------------------------------------------------------------
#   Module configuration
# -----------------------------------------------------------------------------

# The name of the module
DMOD_MODULE_NAME=dmesphosted

# The version of the module
DMOD_MODULE_VERSION=0.1

# The name of the author
DMOD_AUTHOR_NAME=Patryk Kubiak

# The list of C sources
DMOD_CSOURCES=src/dmesphosted.c src/bus.c src/rpc.c src/wifi.c src/queue.c src/frame.c src/pb.c src/rpc_msg.c src/priv.c

# The list of C++ sources
DMOD_CXXSOURCES=

# The list of include directories
DMOD_INC_DIRS=include src

# The list of libraries to link
DMOD_LIBS=dmdrvi dmini dmnetif dmroute dmspi dmgpio dmhaman dmosi

# The list of definitions
DMOD_DEFINITIONS=

# -----------------------------------------------------------------------------
#   List of MAL interfaces implemented by the module
# -----------------------------------------------------------------------------
DMOD_MAL_IMPLS=

# -----------------------------------------------------------------------------
#   List of DIF interfaces implemented by the module
# -----------------------------------------------------------------------------
DMOD_DIF_IMPLS=

# -----------------------------------------------------------------------------
#   Include the dmod app makefile
# -----------------------------------------------------------------------------
include $(DMOD_DMF_LIB_FILE_PATH)
