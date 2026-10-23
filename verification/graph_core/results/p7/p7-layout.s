	.file	"layout.c"
	.text
	.global	admission_bytes
	.section	.rodata
	.align	4
	.type	admission_bytes, @object
	.size	admission_bytes, 4
admission_bytes:
	.word	36776
	.global	record_bytes
	.align	4
	.type	record_bytes, @object
	.size	record_bytes, 4
record_bytes:
	.word	1200
	.global	nodes_bytes
	.align	4
	.type	nodes_bytes, @object
	.size	nodes_bytes, 4
nodes_bytes:
	.word	184
	.global	edges_bytes
	.align	4
	.type	edges_bytes, @object
	.size	edges_bytes, 4
edges_bytes:
	.word	2896
	.global	control_bytes
	.align	4
	.type	control_bytes, @object
	.size	control_bytes, 4
control_bytes:
	.word	7120
	.ident	"GCC: (crosstool-NG esp-14.2.0_20241119) 14.2.0"
