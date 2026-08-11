#include "gog_fixes.hpp"
#include "layout.hpp"

namespace fusioncutter::patches::gog_fixes {
namespace {

// Skips the initializer that crashes under `/norender` and takes the function's cleanup/null-return path.
void add_norender_fix(PatchPlan& plan) {
    plan.checked_write("Force /norender cleanup path", layout::kNoRenderRva,
                       BytePattern::exact(layout::kNoRenderPreimage), layout::kNoRenderReplacement);
}

// Passes the populated `/password` global to the native password setter instead of reading dedicated defaults.
void add_password_fix(PatchPlan& plan) {
    plan.checked_write("Bypass dedicated-defaults password read", layout::kPasswordRva,
                       BytePattern::exact(layout::kPasswordPrefixPreimage), layout::kPasswordPrefixReplacement);
    plan.checked_write("Use /password command-line value", layout::kPasswordPointerRva,
                       BytePattern::exact(layout::kPasswordPointerPreimage),
                       PatchAddress::image_rva(layout::kPlaintextPasswordRva));
    plan.nop("Remove local password reference", layout::kPasswordSuffixRva,
             BytePattern::exact(layout::kPasswordSuffixPreimage));
}

// Publishes r0=1, includes the dedicated host in numPlayers, and reports serverType=2 to Galaxy.
void add_query_metadata_fixes(PatchPlan& plan) {
    plan.checked_write("Publish dedicated host metadata", layout::kPlayerMetadataRva,
                       BytePattern::exact(layout::kPlayerMetadataPreimage), layout::kPlayerMetadataReplacement);
    plan.checked_write("Publish dedicated server type", layout::kServerTypeRva,
                       BytePattern::exact(layout::kServerTypePreimage), layout::kServerTypeReplacement);
}

} // namespace

void GogServerFixes::build_plan(PatchPlan& plan) {
    add_norender_fix(plan);
    add_password_fix(plan);
    add_query_metadata_fixes(plan);
}

} // namespace fusioncutter::patches::gog_fixes
