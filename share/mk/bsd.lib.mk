#	from: @(#)bsd.lib.mk	5.26 (Berkeley) 5/2/91
# $FreeBSD: src/share/mk/bsd.lib.mk,v 1.91.2.15 2002/08/07 16:31:50 ru Exp $
# $DragonFly: src/share/mk/bsd.lib.mk,v 1.9 2005/03/28 03:25:36 dillon Exp $
#

.include <bsd.init.mk>

# Set up the variables controlling shared libraries.  After this section,
# SHLIB_NAME will be defined only if we are to create a shared library.
# SHLIB_LINK will be defined only if we are to create a link to it.
# INSTALL_PIC_ARCHIVE will be defined only if we are to create a PIC archive.
.if defined(NOPIC)
.undef SHLIB_NAME
.undef INSTALL_PIC_ARCHIVE
.else
.if !defined(SHLIB_NAME) && defined(LIB) && defined(SHLIB_MAJOR)
SHLIB_NAME=	lib${LIB}.so.${SHLIB_MAJOR}
.endif
.if defined(SHLIB_NAME) && ${SHLIB_NAME:M*.so.*}
SHLIB_LINK?=	${SHLIB_NAME:R}
.endif
SONAME?=	${SHLIB_NAME}
.endif

.if defined(DEBUG_FLAGS)
CFLAGS+= ${DEBUG_FLAGS}
.endif

.if !defined(DEBUG_FLAGS)
STRIP?=	-s
.endif

.include <bsd.libnames.mk>
.if defined(USEGNUDIR)
USELIBDIR?=${GCCLIBDIR}
USESHLIBDIR?=${GCCSHLIBDIR}
.else
USELIBDIR?=${LIBDIR}
USESHLIBDIR?=${SHLIBDIR}
.endif

# prefer .s to a .c, add .po, remove stuff not used in the BSD libraries
# .So used for PIC object files
.SUFFIXES:
.SUFFIXES: .out .o .po .So .S .s .c .cc .cpp .cxx .m .C .f .y .l

.if !defined(PICFLAG)
PICFLAG=-fpic
.endif

PO_FLAG=-pg

.c.o:
	${CC} ${CFLAGS} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.c.po:
	${CC} ${PO_FLAG} ${CFLAGS} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} -o ${.TARGET}.tmp -X -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.c.So:
	${CC} ${PICFLAG} -DPIC ${CFLAGS} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} ${LDFLAGS} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.cc.o .C.o .cpp.o .cxx.o:
	${CXX} ${CXXFLAGS} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.cc.po .C.po .cpp.po .cxx.po:
	${CXX} ${PO_FLAG} ${CXXFLAGS} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} -o ${.TARGET}.tmp -X -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.cc.So .C.So .cpp.So .cxx.So:
	${CXX} ${PICFLAG} -DPIC ${CXXFLAGS} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} ${LDFLAGS} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.f.o:
	${FC} ${FFLAGS} -o ${.TARGET} -c ${.IMPSRC} 
	@${LD} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.f.po:
	${FC} -pg ${FFLAGS} -o ${.TARGET} -c ${.IMPSRC} 
	@${LD} -o ${.TARGET}.tmp -X -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.f.So:
	${FC} ${PICFLAG} -DPIC ${FFLAGS} -o ${.TARGET} -c ${.IMPSRC}
	@${LD} ${LDFLAGS} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.m.o:
	${OBJC} ${OBJCFLAGS} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.m.po:
	${OBJC} ${OBJCFLAGS} -pg -c ${.IMPSRC} -o ${.TARGET}
	@${LD} -o ${.TARGET}.tmp -X -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.m.So:
	${OBJC} ${PICFLAG} -DPIC ${OBJCFLAGS} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} ${LDFLAGS} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.s.o:
	${CC} -x assembler-with-cpp ${CFLAGS} ${AINC} -c \
	    ${.IMPSRC} -o ${.TARGET}
	@${LD} ${LDFLAGS} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.s.po:
	${CC} -x assembler-with-cpp -DPROF ${CFLAGS} ${AINC} -c \
	    ${.IMPSRC} -o ${.TARGET}
	@${LD} ${LDFLAGS} -o ${.TARGET}.tmp -X -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.s.So:
	${CC} -x assembler-with-cpp ${PICFLAG} -DPIC ${CFLAGS} \
	    ${AINC} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.S.o:
	${CC} ${CFLAGS} ${AINC} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} ${LDFLAGS} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.S.po:
	${CC} -DPROF ${CFLAGS} ${AINC} -c ${.IMPSRC} -o ${.TARGET}
	@${LD} ${LDFLAGS} -o ${.TARGET}.tmp -X -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

.S.So:
	${CC} ${PICFLAG} -DPIC ${CFLAGS} ${AINC} -c ${.IMPSRC} \
	    -o ${.TARGET}
	@${LD} -o ${.TARGET}.tmp -x -r ${.TARGET}
	@mv ${.TARGET}.tmp ${.TARGET}

all: objwarn

.if defined(LIB) && !empty(LIB) || defined(SHLIB_NAME)
OBJS+=  ${SRCS:N*.h:N*.patch:R:S/$/.o/g}
.for _PATCH in ${SRCS:T:N*.no_obj.patch:N*.h.patch:M*.patch}
.for _OBJ in ${_PATCH:R:R:S/$/.o/}
OBJS:=	${OBJS:N${_OBJ}} ${_OBJ}
.endfor
.endfor
.endif

.if defined(LIB) && !empty(LIB)
_LIBS=		lib${LIB}.a

lib${LIB}.a: ${OBJS} ${STATICOBJS}
	@${ECHO} building static ${LIB} library
	rm -f ${.TARGET}
	${AR} cq ${.TARGET} `lorder ${OBJS} ${STATICOBJS} | tsort -q` ${ARADD}
	${RANLIB} ${.TARGET}
.endif

.if !defined(INTERNALLIB)

.if !defined(NOPROFILE) && defined(LIB) && !empty(LIB)
_LIBS+=		lib${LIB}_p.a
POBJS+=		${OBJS:.o=.po} ${STATICOBJS:.o=.po}

lib${LIB}_p.a: ${POBJS}
	@${ECHO} building profiled ${LIB} library
	rm -f ${.TARGET}
	${AR} cq ${.TARGET} `lorder ${POBJS} | tsort -q` ${ARADD}
	${RANLIB} ${.TARGET}
.endif

.if defined(SHLIB_NAME) || \
    defined(INSTALL_PIC_ARCHIVE) && defined(LIB) && !empty(LIB)
SOBJS+=		${OBJS:.o=.So}
.endif

.if defined(SHLIB_NAME)
_LIBS+=		${SHLIB_NAME}

${SHLIB_NAME}: ${SOBJS}
	@${ECHO} building shared library ${SHLIB_NAME}
	rm -f ${.TARGET} ${SHLIB_LINK}
.if defined(SHLIB_LINK)
	ln -fs ${.TARGET} ${SHLIB_LINK}
.endif
	${CC} ${LDFLAGS} -shared -Wl,-x \
	    -o ${.TARGET} -Wl,-soname,${SONAME} \
	    `lorder ${SOBJS} | tsort -q` ${LDADD}
.endif

.if defined(INSTALL_PIC_ARCHIVE) && defined(LIB) && !empty(LIB)
_LIBS+=		lib${LIB}_pic.a

lib${LIB}_pic.a: ${SOBJS}
	@${ECHO} building special pic ${LIB} library
	rm -f ${.TARGET}
	${AR} cq ${.TARGET} ${SOBJS} ${ARADD}
	${RANLIB} ${.TARGET}
.endif

.endif # !defined(INTERNALLIB)

all: ${_LIBS}

.if !defined(NOMAN)
all: _manpages
.endif

_EXTRADEPEND:
	@TMP=_depend$$$$; \
	sed -e 's/^\([^\.]*\).o[ ]*:/\1.o \1.po \1.So:/' < ${DEPENDFILE} \
	    > $$TMP; \
	mv $$TMP ${DEPENDFILE}
.if !defined(NOEXTRADEPEND) && defined(SHLIB_NAME)
.if defined(DPADD) && !empty(DPADD)
	echo ${SHLIB_NAME}: ${DPADD} >> ${DEPENDFILE}
.endif
.endif

.if !target(install)

.if defined(PRECIOUSLIB) && !defined(NOFSCHG)
SHLINSTALLFLAGS+= -fschg
.endif

_INSTALLFLAGS:=	${INSTALLFLAGS}
.for ie in ${INSTALLFLAGS_EDIT}
_INSTALLFLAGS:=	${_INSTALLFLAGS${ie}}
.endfor
_SHLINSTALLFLAGS:=	${SHLINSTALLFLAGS}
.for ie in ${INSTALLFLAGS_EDIT}
_SHLINSTALLFLAGS:=	${_SHLINSTALLFLAGS${ie}}
.endfor

.if !defined(INTERNALLIB)
realinstall: _libinstall
.ORDER: beforeinstall _libinstall
_libinstall:
.if defined(LIB) && !empty(LIB) && !defined(NOINSTALLLIB)
	${INSTALL} -C -o ${LIBOWN} -g ${LIBGRP} -m ${LIBMODE} \
	    ${_INSTALLFLAGS} lib${LIB}.a ${DESTDIR}${USELIBDIR}
.endif
.if !defined(NOPROFILE) && defined(LIB) && !empty(LIB)
	${INSTALL} -C -o ${LIBOWN} -g ${LIBGRP} -m ${LIBMODE} \
	    ${_INSTALLFLAGS} lib${LIB}_p.a ${DESTDIR}${USELIBDIR}
.endif
.if defined(SHLIB_NAME)
	${INSTALL} ${STRIP} -o ${LIBOWN} -g ${LIBGRP} -m ${LIBMODE} \
	    ${_INSTALLFLAGS} ${_SHLINSTALLFLAGS} \
	    ${SHLIB_NAME} ${DESTDIR}${USESHLIBDIR}
.if defined(SHLIB_LINK)
	ln -fs ${SHLIB_NAME} ${DESTDIR}${USESHLIBDIR}/${SHLIB_LINK}
.endif
.endif
.if defined(INSTALL_PIC_ARCHIVE) && defined(LIB) && !empty(LIB)
	${INSTALL} -o ${LIBOWN} -g ${LIBGRP} -m ${LIBMODE} \
	    ${_INSTALLFLAGS} lib${LIB}_pic.a ${DESTDIR}${USELIBDIR}
.endif
.endif !defined(INTERNALLIB)

.include <bsd.nls.mk>
.include <bsd.files.mk>
.include <bsd.incs.mk>
.include <bsd.links.mk>

.if !defined(NOMAN)
realinstall: _maninstall
.ORDER: beforeinstall _maninstall
.endif

.endif

.if !defined(NOMAN)
.include <bsd.man.mk>
.endif

.include <bsd.dep.mk>

.if !exists(${.OBJDIR}/${DEPENDFILE})
.if defined(LIB) && !empty(LIB)
${OBJS} ${STATICOBJS} ${POBJS}: ${SRCS:M*.h}
.endif
.if defined(SHLIB_NAME) || \
    defined(INSTALL_PIC_ARCHIVE) && defined(LIB) && !empty(LIB)
${SOBJS}: ${SRCS:M*.h}
.endif
.endif

.if !target(clean)
clean:
.if defined(CLEANFILES) && !empty(CLEANFILES)
	rm -f ${CLEANFILES}
.endif
.if defined(LIB) && !empty(LIB)
	rm -f a.out ${OBJS} ${OBJS:S/$/.tmp/} ${STATICOBJS}
.endif
.if !defined(INTERNALLIB)
.if !defined(NOPROFILE) && defined(LIB) && !empty(LIB)
	rm -f ${POBJS} ${POBJS:S/$/.tmp/}
.endif
.if defined(SHLIB_NAME) || \
    defined(INSTALL_PIC_ARCHIVE) && defined(LIB) && !empty(LIB)
	rm -f ${SOBJS} ${SOBJS:.So=.so} ${SOBJS:S/$/.tmp/}
.endif
.if defined(SHLIB_NAME)
.if defined(SHLIB_LINK)
	rm -f ${SHLIB_LINK}
.endif
.if defined(LIB) && !empty(LIB)
	rm -f lib${LIB}.so.* lib${LIB}.so
.endif
.endif
.endif !defined(INTERNALLIB)
.if defined(_LIBS) && !empty(_LIBS)
	rm -f ${_LIBS}
.endif
.if defined(CLEANDIRS) && !empty(CLEANDIRS)
	rm -rf ${CLEANDIRS}
.endif
.endif

.include <bsd.obj.mk>

.include <bsd.sys.mk>

