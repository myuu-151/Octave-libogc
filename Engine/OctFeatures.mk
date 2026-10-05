# Parts of Octave a game can leave out of its engine library
# (Source/Engine/EngineFeatures.h says what each is). Included by
# Makefile_GCN and Makefile_Wii, and by a game's makefile, which turns a part
# off by setting OCT_<NAME> := 0 before including this.
#
# Gives:
#   OCT_<NAME>    1 (in) or 0 (left out), for each part, exported
#   OCT_VARIANT   the library's folder suffix: _no<tag> for each part left out
#                 (Build/GCN_nophysics_nonav..., beside the whole one, so games
#                 built either way share one checkout); empty with all in
#   OCT_DEFINES   -DOCT_<NAME>=0/1 for each, for the compiler

# NAME:tag, in the order the folder's name lists them. (The first three keep
# their old tags, so existing libraries keep their folders.)
OCT_FEATURES	:=	PHYSICS:physics NAVIGATION:nav VORBIS:vorbis NETWORK:net VIDEO:video \
			SPLINES:splines SKELETAL:skeletal PARTICLES:particles INSTANCING:instancing \
			TEXT3D:text3d UI_EXTRAS:uiextras CONSOLE:console STATS:stats LUA:lua

oct_name	=	$(word 1,$(subst :, ,$(1)))
oct_tag		=	$(word 2,$(subst :, ,$(1)))
oct_empty	:=
oct_space	:=	$(oct_empty) $(oct_empty)

$(foreach f,$(OCT_FEATURES),$(eval OCT_$(call oct_name,$(f)) ?= 1))
export $(foreach f,$(OCT_FEATURES),OCT_$(call oct_name,$(f)))

OCT_VARIANT	:=	$(subst $(oct_space),,$(foreach f,$(OCT_FEATURES),$(if $(filter 0,$(OCT_$(call oct_name,$(f)))),_no$(call oct_tag,$(f)))))
OCT_DEFINES	:=	$(foreach f,$(OCT_FEATURES),-DOCT_$(call oct_name,$(f))=$(OCT_$(call oct_name,$(f))))
