#include <stdlib.h>
#include "pspgl_internal.h"
#include "pspgl_dlist.h"

/**
 * mask out trigger action registers, or the GE might run amok on context changes...
 * This bitfield is generated from ge_init_state[] with all non-action fields enabled.
 */
uint32_t __pspgl_context_register[256 / 32];

EGLBoolean eglMakeCurrent (EGLDisplay dpy,
			   EGLSurface drawsurf, EGLSurface readsurf,
			   EGLContext ctx)
{
	struct pspgl_context *c = (struct pspgl_context *) ctx;
	struct pspgl_surface *read = (struct pspgl_surface *)readsurf;
	struct pspgl_surface *draw = (struct pspgl_surface *)drawsurf;
	int changed;

	changed = (c != pspgl_curctx);

	if (c) {
		c->refcount++;

		if (draw)
			draw->refcount++;
		if (read)
			read->refcount++;
		if (c->draw)
			eglDestroySurface(dpy, c->draw);
		if (c->read)
			eglDestroySurface(dpy, c->read);
		c->draw = draw;
		c->read = read;
	}

	if (pspgl_curctx) {
		if (changed)
			glFinish();
		eglDestroyContext(dpy, pspgl_curctx);
	}

	pspgl_curctx = c;

	if (c == NULL)
		return EGL_TRUE;

	if (!c->initialized) {
		__pspgl_dlist_init();
		__pspgl_ge_init(c);
		c->initialized = 1;
	}

	if (changed) {
		/* mark all registers and matrices as dirty, we need to
		   rewrite them at init and after context restore... */
		for (int i=0; i<sizeof(c->hw.ge_reg_touched)/sizeof(c->hw.ge_reg_touched[0]); i++)
			c->hw.ge_reg_touched[i] |= __pspgl_context_register[i];
	
		c->projection_stack.flags |= MF_DIRTY;
		c->modelview_stack.flags |= MF_DIRTY;
		c->texture_stack.flags |= MF_DIRTY;
		c->view_stack.flags |= MF_DIRTY;
		for(int i = 0; i < NBONES; i++)
			c->bone_stacks[i].flags |= MF_DIRTY;
	}

	if (!__pspgl_vidmem_setup_write_and_display_buffer(c->draw))
		return EGL_FALSE;

	/* The GE clipping registers depend on the active draw surface.
	 * Restore them even if the GL context itself did not change. */
	sendCommandi(CMD_REGION1, 0);
	sendCommandi(CMD_REGION2,
		     ((c->draw->height - 1) << 10) | (c->draw->width - 1));
	__pspgl_set_scissor();

	return EGL_TRUE;
}

