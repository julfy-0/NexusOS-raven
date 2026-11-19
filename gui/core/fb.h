#ifndef NEXUSOS_FB_H
#define NEXUSOS_FB_H

/* Mark that a GPU driver has taken over the linear framebuffer. */
void fb_mark_hw_ready(int ready);

/* Returns 1 if a GPU driver is backing the framebuffer, 0 otherwise. */
int  fb_is_hw_ready(void);

#endif /* NEXUSOS_FB_H */
