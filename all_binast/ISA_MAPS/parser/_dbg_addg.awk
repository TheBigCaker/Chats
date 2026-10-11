function parse_int(v,    i, c, n, out) {
    sub(/^[[:space:]]+|[[:space:]]+$/, "", v)
    if (v ~ /^0b/) {
        out = 0
        for (i=2; i<=length(v); i++) {
            c = substr(v, i, 1)
            out = out * 2 + (c == "1" ? 1 : 0)
        }
        return out
    }
    if (v ~ /^-?[0-9]+$/) return v + 0
    return 0
}
BEGIN {
    DB = "/data/data/com.termux/files/home/ISA_MAPS/multiplexer/.extract/_aarch64_tmpl_bits.tsv"
    while ((getline line < DB) > 0) {
        n = split(line, f, "\t")
        nm = f[1]
        npar[nm] = f[2] + 0
        np_ = npar[nm]
        for (i=1; i<=np_; i++) par[nm, i] = f[2+i]
        nb_ = f[3+np_] + 0
        nbase[nm] = nb_
        for (i=1; i<=nb_; i++) base[nm, i] = f[3+np_+i]
        nbits[nm] = n - (3+np_+nb_)
        for (i=1; i<=nbits[nm]; i++) spec[nm, i] = f[3+np_+nb_+i]
    }
    close(DB)
    print "AddSubG: npar=" npar["AddSubG"] " nbase=" nbase["AddSubG"]
    for (i=1; i<=npar["AddSubG"]; i++) print "  par[" i "]=" par["AddSubG", i]
    print "  nbits=" nbits["AddSubG"]
    for (i=1; i<=nbits["AddSubG"]; i++) print "  spec[" i "]=" spec["AddSubG", i]
    print "BaseAddSubImm: npar=" npar["BaseAddSubImm"] " nbase=" nbase["BaseAddSubImm"]
    for (i=1; i<=npar["BaseAddSubImm"]; i++) print "  par[" i "]=" par["BaseAddSubImm", i]
    print "  nbits=" nbits["BaseAddSubImm"]
    for (i=1; i<=nbits["BaseAddSubImm"]; i++) print "  spec[" i "]=" spec["BaseAddSubImm", i]
}
