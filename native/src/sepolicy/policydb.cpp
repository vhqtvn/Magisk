#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cil/cil.h>
#include <sepol/debug.h>
#include <sepol/policydb/policydb.h>
#include <sepol/policydb/expand.h>
#include <sepol/policydb/link.h>
#include <sepol/policydb/services.h>
#include <sepol/policydb/avrule_block.h>
#include <sepol/policydb/conditional.h>
#include <sepol/policydb/constraint.h>

#include <base.hpp>
#include <consts.hpp>
#include <sepolicy.hpp>

using namespace std;

static void load_cil(struct cil_db *db, const char *file) {
    char *addr;
    size_t size;
    mmap_ro(file, addr, size);
    cil_add_file(db, (char *) file, addr, size);
    LOGD("cil_add [%s]\n", file);
}

SePolicy SePolicy::from_data(rust::Slice<const uint8_t> data) noexcept {
    LOGD("Load policy from data\n");

    policy_file_t pf;
    policy_file_init(&pf);
    pf.data = (char *) data.data();
    pf.len = data.size();
    pf.type = PF_USE_MEMORY;

    auto db = static_cast<policydb_t *>(malloc(sizeof(policydb_t)));
    if (policydb_init(db) || policydb_read(db, &pf, 0)) {
        LOGE("Fail to load policy from data\n");
        free(db);
        return {};
    }

    return {std::make_unique<sepol_impl>(db)};
}

SePolicy SePolicy::from_file(::rust::Utf8CStr file) noexcept {
    LOGD("Load policy from: %.*s\n", static_cast<int>(file.size()), file.data());

    policy_file_t pf;
    policy_file_init(&pf);
    pf.type = PF_USE_STDIO;
    pf.fp = xfopen(file.data(), "re");

    auto db = static_cast<policydb_t *>(malloc(sizeof(policydb_t)));
    if (policydb_init(db) || policydb_read(db, &pf, 0)) {
        LOGE("Fail to load policy from %s\n", file.data());
        fclose(pf.fp);
        free(db);
        return {};
    }
    fclose(pf.fp);

    return {std::make_unique<sepol_impl>(db)};
}
