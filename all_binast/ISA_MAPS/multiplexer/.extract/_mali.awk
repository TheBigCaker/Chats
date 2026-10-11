function hexdec(h,    i, v, c, d) {
    v = 0
    for (i=1; i<=length(h); i++) {
        c = substr(h, i, 1)
        if (c >= "0" && c <= "9") d = c - "0"
        else if (c >= "a" && c <= "f") d = 10 + (c - "a")
        else if (c >= "A" && c <= "F") d = 10 + (c - "A")
        else d = 0
        v = v * 16 + d
    }
    return v
}
function byte_at(v, n,    i, byte) {
    byte = 0
    for (i=0; i<8; i++) {
        if (int(v / 2^(n*8 + i)) % 2 == 1) byte = byte + 2^i
    }
    return byte
}
{
    name = $1; encs = $5
    v = 0
    if (encs != "") {
        n = split(encs, parts, ",")
        v = hexdec(parts[1])
    }
    line = name
    for (j=0; j<4; j++) line = line "\t" byte_at(v, j)
    for (j=4; j<8; j++) line = line "\t0"
    printf "%s\n", line >> MAT
}
