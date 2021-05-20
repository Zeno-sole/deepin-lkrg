/*
 * pi3's Linux kernel Runtime Guard
 *
 * Component:
 *  - Handle *_JUMP_LABEL self-modifying code.
 *    Hook 'arch_jump_label_transform' function.
 *
 * Notes:
 *  - Linux kernel is heavily consuming *_JUMP_LABEL (if enabled). Most of the
 *    Linux distributions provide kernel with these options compiled. It makes
 *    Linux kernel being self-modifying code. It is very troublesome for this
 *    project. We are relying on comparing hashes from the specific memory
 *    regions and by design self-modifications break this functionality.
 *  - We are hooking into low-level *_JUMP_LABEL functions to be able to
 *    monitor whenever new modification is on the way.
 *
 * Caveats:
 *  - None
 *
 * Timeline:
 *  - Created: 28.I.2019
 *
 * Author:
 *  - Adam 'pi3' Zabrocki (http://pi3.com.pl)
 *
 */

#include "../../../../p_lkrg_main.h"


char p_arch_jump_label_transform_kretprobe_state = 0;
p_lkrg_counter_lock p_jl_lock;

static struct kretprobe p_arch_jump_label_transform_kretprobe = {
    .kp.symbol_name = "arch_jump_label_transform",
    .handler = p_arch_jump_label_transform_ret,
    .entry_handler = p_arch_jump_label_transform_entry,
    .data_size = sizeof(struct p_arch_jump_label_transform_data),
    /* Probe up to 40 instances concurrently. */
    .maxactive = 40,
};


notrace int p_arch_jump_label_transform_entry(struct kretprobe_instance *p_ri, struct pt_regs *p_regs) {

   struct jump_entry *p_tmp = (struct jump_entry *)p_regs_get_arg1(p_regs);
   unsigned long p_addr = p_jump_entry_code(p_tmp);
   struct module *p_module = NULL;
   unsigned long p_flags;

   p_debug_kprobe_log(
          "p_arch_jump_label_transform_entry: comm[%s] Pid:%d\n",current->comm,current->pid);

   p_lkrg_counter_lock_lock(&p_jl_lock, &p_flags);
   p_lkrg_counter_lock_val_inc(&p_jl_lock);
   p_lkrg_counter_lock_unlock(&p_jl_lock, &p_flags);

   p_print_log(P_LKRG_INFO,
               "[JUMP_LABEL] New modification: type[%s] code[0x%lx] target[0x%lx] key[0x%lx]!\n",
               (p_regs_get_arg2(p_regs) == 1) ? "JUMP_LABEL_JMP" :
                                                (p_regs_get_arg2(p_regs) == 0) ? "JUMP_LABEL_NOP" :
                                                                                 "UNKNOWN",
               p_jump_entry_code(p_tmp),
               p_jump_entry_target(p_tmp),
               (unsigned long)p_jump_entry_key(p_tmp));

   if (P_SYM(p_core_kernel_text)(p_addr)) {
      /*
       * OK, *_JUMP_LABEL tries to modify kernel core .text section
       */
      p_db.p_jump_label.p_state = P_JUMP_LABEL_CORE_TEXT;
#ifdef P_LKRG_UNEXPORTED_MODULE_ADDRESS
   } else if ( (p_module = P_SYM(p_module_text_address)(p_addr)) != NULL) {
#else
   } else if ( (p_module = __module_text_address(p_addr)) != NULL) {
#endif
      /*
       * OK, *_JUMP_LABEL tries to modify some module's .text section
       */
      p_db.p_jump_label.p_state = P_JUMP_LABEL_MODULE_TEXT;
      p_db.p_jump_label.p_mod = p_module;
   } else {
      /*
       * FTRACE might generate dynamic trampoline which is not part of .text section.
       * This is not abnormal situation anymore.
       */
      p_print_log(P_LKRG_INFO,
                  "[JUMP_LABEL] Not a .text section! [0x%lx]\n",p_addr);
      p_db.p_jump_label.p_state = P_JUMP_LABEL_WTF_STATE;
   }

   /* A dump_stack() here will give a stack backtrace */
   return 0;
}


notrace int p_arch_jump_label_transform_ret(struct kretprobe_instance *ri, struct pt_regs *p_regs) {

   unsigned int p_tmp,p_tmp2;
   unsigned char p_flag = 0;

   switch (p_db.p_jump_label.p_state) {

      case P_JUMP_LABEL_CORE_TEXT:

         /*
          * We do not require to take any locks neither to copy entire .text section to temporary memory
          * since at this state it is static. Just recompute the hash.
          */
         p_db.kernel_stext.p_hash = p_lkrg_fast_hash((unsigned char *)p_db.kernel_stext.p_addr,
                                                     (unsigned int)p_db.kernel_stext.p_size);

#if defined(P_LKRG_JUMP_LABEL_STEXT_DEBUG)
         memcpy(p_db.kernel_stext_copy,p_db.kernel_stext.p_addr,p_db.kernel_stext.p_size);
         p_db.kernel_stext_copy[p_db.kernel_stext.p_size] = 0;
#endif

         p_print_log(P_LKRG_INFO,
                     "[JUMP_LABEL] Updating kernel core .text section hash!\n");

         break;

      case P_JUMP_LABEL_MODULE_TEXT:

         for (p_tmp = 0; p_tmp < p_db.p_module_list_nr; p_tmp++) {
            if (p_db.p_module_list_array[p_tmp].p_mod == p_db.p_jump_label.p_mod) {
               /*
                * OK, we found this module on our internal tracking list.
                * Update it's hash
                */

               p_print_log(P_LKRG_INFO,
                      "[JUMP_LABEL] Updating module's core .text section hash module[%s : 0x%lx]!\n",
                      p_db.p_module_list_array[p_tmp].p_name,
                      (unsigned long)p_db.p_module_list_array[p_tmp].p_mod);

               p_db.p_module_list_array[p_tmp].p_mod_core_text_hash =
                    p_lkrg_fast_hash((unsigned char *)p_db.p_module_list_array[p_tmp].p_module_core,
                                     (unsigned int)p_db.p_module_list_array[p_tmp].p_core_text_size);
               /*
                * Because we have modified individual module's hash, we need to update
                * 'global' module's list hash as well
                */
               p_db.p_module_list_hash = p_lkrg_fast_hash((unsigned char *)p_db.p_module_list_array,
                                                          (unsigned int)p_db.p_module_list_nr * sizeof(p_module_list_mem));

               /*
                * Because we update module's .text section hash we need to update KOBJs as well.
                */
               for (p_tmp2 = 0; p_tmp2 < p_db.p_module_kobj_nr; p_tmp2++) {
                  if (p_db.p_module_kobj_array[p_tmp2].p_mod == p_db.p_jump_label.p_mod) {
                     p_db.p_module_kobj_array[p_tmp2].p_mod_core_text_hash =
                                      p_db.p_module_list_array[p_tmp].p_mod_core_text_hash;
                     p_flag = 1;
                     break;
                  }
               }

               if (!p_flag) {
                  p_print_log(P_LKRG_ERR,
                              "[JUMP_LABEL] Updated module's list hash for module[%s] but can't find the same module in KOBJs list!\n",
                              p_db.p_module_list_array[p_tmp].p_name);
                  p_print_log(P_LKRG_INFO,"module[%s : 0x%lx]!\n",
                              p_db.p_module_list_array[p_tmp].p_name,
                              (unsigned long)p_db.p_module_list_array[p_tmp].p_mod);
               } else {

                  /*
                   * Because we have modified individual module's hash, we need to update
                   * 'global' module's list hash as well
                   */
                  p_db.p_module_kobj_hash = p_lkrg_fast_hash((unsigned char *)p_db.p_module_kobj_array,
                                                             (unsigned int)p_db.p_module_kobj_nr * sizeof(p_module_kobj_mem));
               }
               break;
            }
         }
         break;

      case P_JUMP_LABEL_WTF_STATE:
      default:
         /*
          * FTRACE might generate dynamic trampoline which is not part of .text section.
          * This is not abnormal situation anymore.
          */
         break;
   }

   p_db.p_jump_label.p_state = P_JUMP_LABEL_NONE;

   p_lkrg_counter_lock_val_dec(&p_jl_lock);

   return 0;
}


int p_install_arch_jump_label_transform_hook(void) {

   int p_tmp;

   p_lkrg_counter_lock_init(&p_jl_lock);

   if ( (p_tmp = register_kretprobe(&p_arch_jump_label_transform_kretprobe)) != 0) {
      p_print_log(P_LKRG_ERR, "[kretprobe] register_kretprobe() for <%s> failed! [err=%d]\n",
                  p_arch_jump_label_transform_kretprobe.kp.symbol_name,
                  p_tmp);
      return P_LKRG_GENERAL_ERROR;
   }
   p_print_log(P_LKRG_INFO, "Planted [kretprobe] <%s> at: 0x%lx\n",
               p_arch_jump_label_transform_kretprobe.kp.symbol_name,
               (unsigned long)p_arch_jump_label_transform_kretprobe.kp.addr);
   p_arch_jump_label_transform_kretprobe_state = 1;

   return P_LKRG_SUCCESS;
}


void p_uninstall_arch_jump_label_transform_hook(void) {

   if (!p_arch_jump_label_transform_kretprobe_state) {
      p_print_log(P_LKRG_INFO, "[kretprobe] <%s> at 0x%lx is NOT installed\n",
                  p_arch_jump_label_transform_kretprobe.kp.symbol_name,
                  (unsigned long)p_arch_jump_label_transform_kretprobe.kp.addr);
   } else {
      unregister_kretprobe(&p_arch_jump_label_transform_kretprobe);
      p_print_log(P_LKRG_INFO, "Removing [kretprobe] <%s> at 0x%lx nmissed[%d]\n",
                  p_arch_jump_label_transform_kretprobe.kp.symbol_name,
                  (unsigned long)p_arch_jump_label_transform_kretprobe.kp.addr,
                  p_arch_jump_label_transform_kretprobe.nmissed);
      p_arch_jump_label_transform_kretprobe_state = 0;
   }
}
