function flush_tpl(    line, i, k) {
    if (tpl_name == "") return
    print "FLUSH:", tpl_name
    tpl_name = ""; tpl_nparams = 0; delete tpl_params; delete bits_map
}
BEGIN { in_tpl = 0; brace_depth = 0; print "BEGIN fired" }
{
    s = $0
    print "PROCESS:", s, "in_tpl_before=", in_tpl
    if (!in_tpl && match(s, /^[[:space:]]*(class|multiclass)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[ \t]*</)) {
        flush_tpl()
        print "  MATCHED HEADER"
        tpl_name = "X"
        in_tpl = 1
        brace_depth = 0
    }
}
