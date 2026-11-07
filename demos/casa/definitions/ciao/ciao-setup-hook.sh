ciaoSetup() {
  export ASCDS_INSTALL=@out@
  export PFILES="${PFILES:-${HOME:-/tmp}/cxcds_param4;}@out@/param"
}

ciaoSetup
