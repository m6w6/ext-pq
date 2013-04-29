#!/usr/bin/awk -f

BEGIN {
	printf "#ifndef PHP_PQ_TYPE\n"
	printf "# define PHP_PQ_TYPE(t,o)\n"
	printf "#endif\n"
}

END {
	printf "#ifndef PHP_PQ_TYPE_IS_ARRAY\n"
	printf "# define PHP_PQ_TYPE_IS_ARRAY(oid) (\\\n\t\t0 \\\n"
	for (oid in arrays) {
		printf "\t||\t((oid) == %d) \\\n", oid
	}
	printf ")\n#endif\n"
	printf "#ifndef PHP_PQ_TYPE_OF_ARRAY\n"
	printf "# define PHP_PQ_TYPE_OF_ARRAY(oid) ("
	for (oid in arrays) {
		printf "\\\n\t(oid) == %d ? %s : ", oid, arrays[oid]
	}
	printf "0 \\\n)\n#endif\n"
}

/^DATA/ {
	oid = $4
	name = toupper($6)
	atypoid = $17
	if (sub("^_", "", name)) {
		arrays[oid] = atypoid
		name = name "ARRAY"
	}
	printf "#ifndef PHP_PQ_OID_%s\n", name
	printf "# define PHP_PQ_OID_%s %d\n", name, oid
	printf "#endif\n"
	printf "PHP_PQ_TYPE(\"%s\", %d)\n", name, oid
}
