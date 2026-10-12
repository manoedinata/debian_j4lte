project(
	'unudhcpd',
	'c',
	version : '0.1',
	default_options : ['warning_level=3'],
	meson_version : '>=0.56.0',
)
add_project_arguments('-DVERSION="@0@"'.format(meson.project_version()), language : ['c'])

src = [
	'main.c',
	'server.c',
]

exe = executable(
	'unudhcpd',
	src,
	install : true
)

subdir('tests')
