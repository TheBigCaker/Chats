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
function bintodec(b,    i, v, c) {
    v = 0
    for (i=1; i<=length(b); i++) {
        c = substr(b, i, 1)
        v = v * 2 + (c == "1" ? 1 : 0)
    }
    return v
}
function hash_str(s,    i, h, c) {
    h = 0
    for (i=1; i<=length(s); i++) {
        c = substr(s, i, 1) + 0
        h = (h * 31 + c) - int((h * 31 + c) / 4294967296) * 4294967296
        if (h < 0) h = h + 4294967296
    }
    return h
}
function byte_at(v, n,    i, byte) {
    byte = 0
    for (i=0; i<8; i++) {
        if (int(v / 2^(n*8 + i)) % 2 == 1) byte = byte + 2^i
    }
    return byte
}
{
    name = $1; enc1 = $5
    v = 0
    if (enc1 ~ /^0b/) v = bintodec(substr(enc1, 3))
    else if (enc1 ~ /^0x/) v = hexdec(substr(enc1, 3))
    else if (enc1 ~ /^[0-9]+$/) v = enc1 + 0
    h = hash_str(name)
    packed = v + h * 256
    line = name
    for (j=0; j<8; j++) line = line "\t" byte_at(packed, j)
    printf "%s\n", line >> MAT
}
