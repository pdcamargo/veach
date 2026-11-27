import sys, urllib.request, urllib.parse, time
out = sys.argv[1]
jds = ["2378496.5", "2415020.5", "2433282.5", "2451545.0", "2460847.5", "2469807.5"]
targets = [("1", "500@10"), ("2", "500@10"), ("3", "500@10"), ("4", "500@10"), ("5", "500@10"), ("6", "500@10"),
           ("7", "500@10"), ("8", "500@10"), ("399", "500@10"), ("301", "500@399")]
for cmd, center in targets:
    q = {"format": "text", "COMMAND": f"'{cmd}'", "EPHEM_TYPE": "'VECTORS'", "CENTER": f"'{center}'",
         "REF_PLANE": "'ECLIPTIC'", "REF_SYSTEM": "'ICRF'", "TLIST": "'" + " ".join(jds) + "'", "TIME_TYPE": "'TDB'",
         "VEC_TABLE": "'1'", "OUT_UNITS": "'KM-S'", "CSV_FORMAT": "'YES'", "OBJ_DATA": "'NO'", "VEC_LABELS": "'NO'"}
    url = "https://ssd.jpl.nasa.gov/api/horizons.api?" + urllib.parse.urlencode(q, safe="'@")
    txt = urllib.request.urlopen(url, timeout=60).read().decode()
    open(f"{out}/h_{cmd}.txt", "w").write(txt)
    time.sleep(0.5)
print("ok")
