internal_config.set10('HAVE_DRM_COLOR_PIPELINE', cc.compiles('''
#include <xf86drm.h>
#include <xf86drmMode.h>
int main(void) {
    struct drm_color_ctm_3x4 matrix = {0};
    struct drm_color_lut32 lut = {0};
    return DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE + DRM_MODE_OBJECT_COLOROP +
        DRM_COLOROP_1D_CURVE + DRM_COLOROP_1D_LUT + DRM_COLOROP_CTM_3X4 +
        DRM_COLOROP_MULTIPLIER + matrix.matrix[0] + lut.red;
}
''', dependencies: drm, name: 'DRM color pipeline UAPI'))

hwdata = dependency(
	'hwdata',
	required: 'drm' in backends,
	native: true,
	not_found_message: 'Required for the DRM backend.',
)

libdisplay_info = dependency(
	'libdisplay-info',
	version: '>=0.2.0',
	required: 'drm' in backends,
	fallback: 'libdisplay-info',
	not_found_message: 'Required for the DRM backend.',
)

libliftoff = dependency(
	'libliftoff',
	version: '>=0.4.0',
	fallback: 'libliftoff',
	required: get_option('libliftoff'),
)

if not (hwdata.found() and libdisplay_info.found() and features['session'])
	subdir_done()
endif

hwdata_dir = hwdata.get_variable(pkgconfig: 'pkgdatadir')
pnpids_c = custom_target(
	'pnpids.c',
	output: 'pnpids.c',
	input: files(hwdata_dir / 'pnp.ids'),
	feed: true,
	capture: true,
	command: files('gen_pnpids.sh'),
)
wlr_files += pnpids_c

wlr_files += files(
	'atomic.c',
	'color_pipeline.c',
	'backend.c',
	'drm.c',
	'fb.c',
	'legacy.c',
	'monitor.c',
	'properties.c',
	'renderer.c',
	'util.c',
)

if libliftoff.found()
	wlr_files += files('libliftoff.c')
	internal_config.set10('HAVE_LIBLIFTOFF_0_5', libliftoff.version().version_compare('>=0.5.0'))
endif

features += { 'drm-backend': true }
internal_features += { 'libliftoff': libliftoff.found() }
wlr_deps += libdisplay_info
wlr_deps += libliftoff
