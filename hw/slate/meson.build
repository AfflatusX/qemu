# only build slate board if cairo lib is found.
if cairo.found()
    softmmu_ss.add(when: 'CONFIG_SLATE', if_true: cairo)

    specific_ss.add(when: ['CONFIG_SLATE'], if_true: cairo)
    arm_ss.add(when: 'CONFIG_SLATE', if_true: files('board.c', 'ram_disp.c', 'nema.c', 'timer.c', 'slate_utils.c'))
endif
