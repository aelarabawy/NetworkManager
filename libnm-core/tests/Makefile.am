if ENABLE_TESTS

certsdir = $(srcdir)/certs

AM_CPPFLAGS = \
	-I${top_srcdir}/include \
	-I$(top_srcdir)/libnm-core \
	-I$(top_builddir)/libnm-core \
	-DNETWORKMANAGER_COMPILATION \
	-DNM_VERSION_MAX_ALLOWED=NM_VERSION_NEXT_STABLE \
	$(GLIB_CFLAGS) \
	-DTEST_CERT_DIR=\"$(certsdir)\"

noinst_PROGRAMS =		\
	test-compare		\
	test-crypto		\
	test-general		\
	test-secrets		\
	test-setting-8021x	\
	test-setting-dcb	\
	test-settings-defaults

LDADD = \
	$(top_builddir)/libnm-core/libnm-core.la \
	$(GLIB_LIBS)

TESTS = $(noinst_PROGRAMS)

endif

# test-cert.p12 created with:
#
# openssl pkcs12 -export \
#	-in test_key_and_cert.pem \
#	-inkey test_key_and_cert.pem \
#	-certfile test_ca_cert.pem \
#	-name "test-pkcs12" \
#	-out test-cert.p12

EXTRA_DIST =                              \
	certs/test_ca_cert.pem            \
	certs/test_ca_cert.der            \
	certs/test_key_and_cert.pem       \
	certs/test-cert.p12               \
	certs/test2_ca_cert.pem           \
	certs/test2_key_and_cert.pem      \
	certs/test2-cert.p12              \
	certs/ca-no-ending-newline.pem    \
	certs/test-key-only.pem           \
	certs/test-key-only-decrypted.der \
	certs/test-key-only-decrypted.pem \
	certs/pkcs8-enc-key.pem           \
	certs/pkcs8-noenc-key.pem         \
	certs/pkcs8-decrypted.der         \
	certs/test-aes-key.pem

