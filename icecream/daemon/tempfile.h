
#ifdef __cplusplus
extern "C" {
#endif

int dcc_make_tmpnam(const char *prefix,
                    const char *suffix,
                    char *name_ret);

const char * dcc_preproc_exten(const char *e);

#ifdef __cplusplus
}
#endif
