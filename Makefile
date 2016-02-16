#------------------------------------------------------------------------------
# Copyright (c) 2014-2015 Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 1. Redistributions of source code must retain the above copyright notice,
# this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
# 3. Neither the name of the copyright holder nor the names of its
# contributors may be used to endorse or promote products derived from this
# software without specific prior written permission.
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#------------------------------------------------------------------------------
# ...<root>/Makefile

TOPDIR := ${shell pwd}
include $(TOPDIR)/Makefile.inc

#  VERBOSE==1:  Echo commands
#  VERBOSE!=1:  Do not echo commands
ifeq ($(VERBOSE),1)
export Q :=
else
export Q := @
endif

# Sub-directories
SUBDIRS  = src

all: $(SUBDIRS)

.PHONY: all nothing depend clean distclean

# SDIR_template <SubDir>
# recursively make <SubDir>
define SDIR_template0
$(1): force_look
	$(Q) $(MAKE) -C $(1) TOPDIR="$(TOPDIR)"
endef

# SDIR_template <SubDir> <Target>
# recursively make <Target> in <SubDir>
define SDIR_template
$(1)_$(2):
	$(Q) $(MAKE) -C $(1) $(2) TOPDIR="$(TOPDIR)"
endef

# create generic dependencies for all subdirs. e.g.:
# create-tftf:
# 	make -C create-tftf
#  :
# create-ffff:
# 	make -C create-ffff
$(foreach SDIR, $(SUBDIRS), $(eval $(call SDIR_template0,$(SDIR))))

# create targeted dependencies for all subdirs. e.g.:
# create-tftf_clean:
# 	make -C create-tftf clean
#  :
# create-ffff_clean:
# 	make -C create-ffff clean
$(foreach SDIR, $(SUBDIRS), $(eval $(call SDIR_template,$(SDIR),clean)))


nothing:

install: all
	$(INSTALL) $(foreach BIN, $(INSTALL_BINS), $(BINDIR)/$(BIN)) \
		$(foreach SCRIPT, $(INSTALL_SCRIPTS), $(SCRIPTDIR)/$(SCRIPT)) \
		-t $(INSTALLDIR)/bin

uninstall:
	$(RM) $(foreach BIN, $(INSTALL_BINS), $(INSTALLDIR)/bin/$(BIN)) \
		$(foreach SCRIPT, $(INSTALL_SCRIPTS), $(INSTALLDIR)/bin/$(SCRIPT))

# clean the tree by dependencies on all of the xxx_clean dependencies declared above
clean: $(foreach SDIR, $(SUBDIRS), $(SDIR)_clean)
	- rm -rf bin/*

force_look :
	true

