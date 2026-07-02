#include "echem_core/technique.h"
#include "echem_core/dpv.h"
#include <string.h>

static const technique_t *s_registry[TECHNIQUE_REGISTRY_MAX];
static int                s_count = 0;

int technique_register(const technique_t *t)
{
    if (t == NULL || s_count >= TECHNIQUE_REGISTRY_MAX) {
        return -1;
    }
    s_registry[s_count++] = t;
    return 0;
}

const technique_t *technique_find(const char *name)
{
    if (name == NULL) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i]->name, name) == 0) {
            return s_registry[i];
        }
    }
    return NULL;
}

void technique_registry_init(void)
{
    s_count = 0;
    /* DPV is the default / only technique in v1. */
    technique_register(dpv_get_technique());

    /*
     * CV, LSV, SWV, NPV: registered here when implemented (post DPV publish).
     * technique_register(cv_get_technique());
     * technique_register(lsv_get_technique());
     * technique_register(swv_get_technique());
     * technique_register(npv_get_technique());
     */
}
