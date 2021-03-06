# Created by: Matthias Andree <matthias.andree@gmx.de>
# $FreeBSD: head/security/openvpn/Makefile 310340 2013-01-13 21:35:06Z mandree $

PORTNAME=	openvpn
DISTVERSION=	2.3.0
PORTREVISION=	1
CATEGORIES=	security net
MASTER_SITES=	http://swupdate.openvpn.net/community/releases/ \
		http://build.openvpn.net/downloads/releases/

MAINTAINER=	mandree@FreeBSD.org
COMMENT=	Secure IP/Ethernet tunnel daemon

LICENSE=	GPLv2

CONFLICTS_INSTALL=	openvpn-2.[!3].* openvpn-[!2].* openvpn-beta-[0-9]* openvpn-devel-[0-9]*

GNU_CONFIGURE=	yes
USE_OPENSSL=	yes
USE_XZ=		yes
# let OpenVPN's configure script pick up the libraries
CPPFLAGS+=	-I${LOCALBASE}/include
LDFLAGS+=	-L${LOCALBASE}/lib

# PolarSSL missing: OpenVPN does not currently compile with PolarSSL 1.2.0+
OPTIONS_DEFINE=		PW_SAVE PKCS11 EASYRSA
OPTIONS_DEFAULT=	PW_SAVE
PW_SAVE_DESC=	Interactive passwords may be read from a file
PKCS11_DESC=	Use security/pkcs11-helper
EASYRSA_DESC=	Install security/easy-rsa RSA helper package

.include <bsd.port.options.mk>

.if ${PORT_OPTIONS:MDOCS}
INSTALL_TARGET=	install
.else
INSTALL_TARGET=	install-exec install-man
.endif

.if ${PORT_OPTIONS:MEASYRSA}
RUN_DEPENDS+=	easy-rsa>=0:${PORTSDIR}/security/easy-rsa
.endif

INSTALL_TARGET+=	mandir=${MANPREFIX}/man
MAN8=		openvpn.8

USE_RC_SUBR=	openvpn
USE_LDCONFIG=	${PREFIX}/lib

SUB_FILES=	pkg-message
SUB_LIST+=	OSVERSION=${OSVERSION}

.include <bsd.port.pre.mk>

.ifdef (LOG_OPENVPN)
CFLAGS+=	-DLOG_OPENVPN=${LOG_OPENVPN}
.endif

LIB_DEPENDS+=	lzo2:${PORTSDIR}/archivers/lzo2

.if ${PORT_OPTIONS:MPW_SAVE}
CONFIGURE_ARGS+=	--enable-password-save
.else
CONFIGURE_ARGS+=	--disable-password-save
.endif

.if ${PORT_OPTIONS:MPKCS11}
LIB_DEPENDS+=	pkcs11-helper:${PORTSDIR}/security/pkcs11-helper
.else
CONFIGURE_ARGS+=	--disable-pkcs11
.endif

pre-configure:
.ifdef (LOG_OPENVPN)
	@${ECHO} "Building with LOG_OPENVPN=${LOG_OPENVPN}"
.else
	@${ECHO} ""
	@${ECHO} "You may use the following build options:"
	@${ECHO} ""
	@${ECHO} "      LOG_OPENVPN={Valid syslog facility, default LOG_DAEMON}"
	@${ECHO} "      EXAMPLE:  make LOG_OPENVPN=LOG_DAEMON"
	@${ECHO} ""
.endif

post-build:
	@# self-tests here
.if !defined(WITHOUT_CHECK)
	@${ECHO} ; ${ECHO} "### Note that you can skip these lengthy selftests with WITHOUT_CHECK=yes ###" ; ${ECHO}
	cd ${WRKSRC} && ${SETENV} ${MAKE_ENV} ${MAKE} ${MAKE_FLAGS} ${MAKEFILE} ${_MAKE_JOBS} ${MAKE_ARGS} check
.endif

post-install:
	${MKDIR} ${PREFIX}/lib
.if ${PORT_OPTIONS:MDOCS}
	${MKDIR} ${DOCSDIR}
.for docs in AUTHORS COPYING COPYRIGHT.GPL ChangeLog INSTALL \
	PORTS README
	${INSTALL_DATA} ${WRKSRC}/${docs} ${DOCSDIR}/
.endfor
.for dir in sample/sample-config-files
	${RM} -f ${WRKSRC}/${dir}/*.orig
	${MKDIR} ${DOCSDIR}/${dir}
	${FIND} ${WRKSRC}/${dir}/ -maxdepth 1 -type f -exec ${INSTALL_DATA} \{\} ${DOCSDIR}/${dir} \;
.endfor
.for dir in sample/sample-scripts
	${MKDIR} ${DOCSDIR}/${dir}
	${FIND} ${WRKSRC}/${dir}/ -maxdepth 1 -type f -exec ${INSTALL_SCRIPT} \{\} ${DOCSDIR}/${dir} \;
.endfor
.else
	-@${RMDIR} ${DOCSDIR}
.endif
	@${CAT} ${PKGMESSAGE}

.include <bsd.port.post.mk>
