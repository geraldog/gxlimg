#ifndef _SSL_H_
#define _SSL_H_

#include <openssl/evp.h>
#include <openssl/err.h>

#define SSLERR(ret, ...) do						\
{									\
	char __sslerrbuf[256];						\
	fprintf(stderr, __VA_ARGS__);					\
	ret = -ERR_get_error();						\
	ERR_error_string_n(-ret, __sslerrbuf, sizeof(__sslerrbuf));	\
	fprintf(stderr, "%s\n", __sslerrbuf);				\
} while(0)

#if OPENSSL_VERSION_NUMBER < 0x10100000L
static inline void *OPENSSL_zalloc(size_t num)
{
	void *ret = OPENSSL_malloc(num);

	if (ret != NULL)
		memset(ret, 0, num);
	return ret;
}

static inline EVP_MD_CTX *EVP_MD_CTX_new(void)
{
	return OPENSSL_zalloc(sizeof(EVP_MD_CTX));
}

static inline void EVP_MD_CTX_free(EVP_MD_CTX *ctx)
{
	EVP_MD_CTX_cleanup(ctx);
	OPENSSL_free(ctx);
}
#endif

#endif
