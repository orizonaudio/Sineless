// The neutral name and mark that builds without the Orizon brand files show
// (SINELESS_BRANDING=OFF, see ui/src/brand/README.md). A fork puts its own here.
const mark =
  '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" aria-hidden="true">' +
  '<circle cx="32" cy="32" r="26" fill="none" stroke="currentColor" stroke-width="8"/></svg>';

// No vendor: without the brand there is no vendor's site, and About Us is not shown.
export const brand = { name: "Scope", vendor: "", mark };
