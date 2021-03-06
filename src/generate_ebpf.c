/*
 * Copyright 2016-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * generate_ebpf.c -- generate ebpf code
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ebpf/trace.h"
#include "ebpf/ebpf_file_set.h"

#include "vltrace.h"
#include "utils.h"
#include "ebpf_syscalls.h"
#include "generate_ebpf.h"
#include "syscalls_numbers.h"

static const char *Tmpl_str[MAX_STR_ARG] = {"STR1", "STR2", "STR3"};

static char *
get_template(unsigned sc_num)
{
	char *text = NULL;

	if (Args.ff_mode == E_FF_FULL) {
		switch (sc_num) {
		case __NR_fork:
		case __NR_vfork:
		case __NR_clone:
			text = load_file_no_cr(ebpf_fork_file);
			goto replace_skip_STRX;
		case __NR_exit:
		case __NR_exit_group:
			text = load_file_no_cr(ebpf_exit_file);
			goto replace_skip_STRX;
		}
	}

	int nstr = Syscall_array[sc_num].nstrings;

	if (nstr > MAX_STR_ARG) {
		WARNING("syscall '%s' has more than %i string arguments,\n"
			"\t only first %i of them will be printed\n",
			Syscall_array[sc_num].syscall_name,
			MAX_STR_ARG, MAX_STR_ARG);

		nstr = MAX_STR_ARG;
	}

	const char *file = ebpf_file_table[nstr][Args.fnr_mode];
	text = load_file_no_cr(file);
	if (text == NULL) {
		ERROR("cannot load the file: %s", file);
		return NULL;
	}

	/* replace STRX */
	for (int i = 0; i < nstr; i++) {
		if (str_replace_with_char(text, Tmpl_str[i],
					Syscall_array[sc_num].positions[i]))
			goto exit_on_error;
	}

replace_skip_STRX:
	if (str_replace_all(&text, "SYSCALL_NR", Syscall_array[sc_num].num_str))
		goto exit_on_error;

	if (str_replace_all(&text, "SYSCALL_NAME_filled_for_replace",
			Syscall_array[sc_num].syscall_name))
		goto exit_on_error;

	return text;

exit_on_error:
	free(text);
	return NULL;
}

/*
 * generate_ebpf_kp_mask -- generate eBPF syscall handlers specific
 *                          for Kprobes with given mask
 */
static int
generate_ebpf_kp_mask(FILE *ts, unsigned mask)
{
	char *text = NULL;
	size_t fw_res;

	for (unsigned i = 0; i < SC_TBL_SIZE; i++) {

		if (!Syscall_array[i].available)
			continue;

		if (mask && ((mask & Syscall_array[i].mask) == 0))
			continue;

		text = get_template(i);
		if (text == NULL) {
			ERROR("no template found for syscall: '%s'",
				Syscall_array[i].syscall_name);
			return -1;
		}

		fw_res = fwrite(text, strlen(text), 1, ts);
		if (fw_res < 1) {
			perror("fwrite");
			free(text);
			return -1;
		}

		free(text);
	}
	return 0;
}

/*
 * generate_ebpf_tp_all -- generate eBPF syscall handlers
 *                         specific for Tracepoints
 */
static int
generate_ebpf_tp_all(FILE *ts)
{
	char *text = load_file_no_cr(ebpf_tracepoints_file);
	int ret = 0;

	size_t fw_res = fwrite(text, strlen(text), 1, ts);
	if (fw_res < 1) {
		perror("fwrite");
		ret = -1;
	}

	free(text);

	return ret;
}

/*
 * generate_ebpf_all_kp_tp -- generate eBPF syscall handlers
 *                            specific for Kprobes and Tracepoints
 */
static int
generate_ebpf_all_kp_tp(FILE *ts)
{
	int ret;

	ret = generate_ebpf_kp_mask(ts, 0);
	if (ret)
		return ret;

	return generate_ebpf_tp_all(ts);
}

/*
 * generate_ebpf -- parse and process expression
 */
char *
generate_ebpf()
{
	char *text = NULL;
	size_t text_size = 0;
	int ret = -1;

	FILE *ts = open_memstream(&text, &text_size);
	if (ts == NULL)
		return NULL;

	char *head_file = load_file(ebpf_head_file);
	if (head_file == NULL)
		goto error_load_file_1;

	char *trace_h_file = load_file_no_cr(ebpf_trace_h_file);
	if (trace_h_file == NULL)
		goto error_load_file_2;

	if (Args.n_str_packets == 2 &&
	    str_replace_all(&head_file,
			"READ_AND_SUBMIT_it_will_be_removed_N_MINUS_2_PACKETS",
			"READ_AND_SUBMIT_N_MINUS_2_PACKETS"))
		goto error_load_file_2;

	if (str_replace_all(&head_file, "#include \"trace.h\"", trace_h_file))
		goto error_str_replace_all;

	free(trace_h_file);
	trace_h_file = NULL;

	size_t fw_res = fwrite(head_file, strlen(head_file), 1, ts);
	if (fw_res < 1) {
		perror("fwrite");
		goto error_load_file_2;
	}

	free(head_file);
	head_file = NULL;

	if (Args.expr == NULL) {
		NOTICE("defaulting to 'all'");
		ret = generate_ebpf_all_kp_tp(ts);
	} else if (strcasecmp(Args.expr, "all") == 0) {
		ret = generate_ebpf_all_kp_tp(ts);
	} else if (strcasecmp(Args.expr, "kp-all") == 0) {
		ret = generate_ebpf_kp_mask(ts, 0);
	} else if (strcasecmp(Args.expr, "kp-file") == 0) {
		ret = generate_ebpf_kp_mask(ts, EM_str_1);
	} else if (strcasecmp(Args.expr, "kp-desc") == 0) {
		ret = generate_ebpf_kp_mask(ts, EM_fd_1);
	} else if (strcasecmp(Args.expr, "kp-fileio") == 0) {
		ret = generate_ebpf_kp_mask(ts, EM_str_1 | EM_str_2 | EM_fd_1);
	} else {
		ERROR("unknown option: '%s'", Args.expr);
	}

error_str_replace_all:
	if (trace_h_file)
		free(trace_h_file);
error_load_file_2:
	if (head_file)
		free(head_file);
error_load_file_1:
	fclose(ts);
	if (ret) {
		free(text);
		return NULL;
	}
	return text;
}

/*
 * apply_process_attach_code -- apply process-attach code
 *                              to generated code with handlers
 */
int
apply_process_attach_code(char **const pbpf_str)
{
	char strpid[64];
	int snp_res;
	char *loaded_file;
	int pid;
	int ret = -1;

	if (0 < Args.pid) {
		/* traced pid */
		pid = Args.pid;

		snp_res = snprintf(strpid, sizeof(strpid), "%d", pid);
		assert(snp_res > 0);
		(void) snp_res;

		loaded_file = load_pid_check_hook(Args.ff_mode);
		if (loaded_file == NULL)
			return -1;

		if (str_replace_all(&loaded_file, "TRACED_PID", strpid))
			goto exit_free;
	} else {
		/* my own pid */
		pid = getpid();
		NOTICE("will not trace my own PID %i (0x%X)", pid, pid);

		snp_res = snprintf(strpid, sizeof(strpid), "%d", pid);
		assert(snp_res > 0);
		(void) snp_res;

		loaded_file = load_file_no_cr(ebpf_pid_own_file);
		if (loaded_file == NULL)
			return -1;

		if (str_replace_all(&loaded_file, "MY_OWN_PID", strpid))
			goto exit_free;
	}

	if (str_replace_all(pbpf_str, "PID_CHECK_HOOK", loaded_file))
		goto exit_free;

	free(loaded_file);
	loaded_file = NULL;

	if (Args.fnr_mode == E_STR_FULL_CONST_N && Args.n_str_packets > 2) {
		loaded_file = load_file_no_cr(ebpf_const_string_mode);
		if (loaded_file == NULL)
			return -1;
		if (str_replace_many(pbpf_str,
					"READ_AND_SUBMIT_N_MINUS_2_PACKETS",
					loaded_file, Args.n_str_packets - 2))
			goto exit_free;
	}

	if (Args.fnr_mode == E_STR_FULL && Args.n_str_packets > 2) {
		loaded_file = load_file_no_cr(ebpf_full_string_mode);
		if (loaded_file == NULL)
			return -1;
		if (str_replace_many(pbpf_str,
					"READ_AND_SUBMIT_N_MINUS_2_PACKETS",
					loaded_file, Args.n_str_packets - 2))
			goto exit_free;
	}

	ret = 0;

exit_free:
	if (loaded_file)
		free(loaded_file);
	return ret;
}

/*
 * fprint_ebpf_code_with_debug_marks - print ebpf code with marks
 *                                     for debug reason
 */
void
fprint_ebpf_code_with_debug_marks(FILE *f, const char *bpf_str)
{
	fprintf(f, "\t>>>>> Generated eBPF code <<<<<\n");

	if (bpf_str) {
		size_t fw_res;

		fw_res = fwrite(bpf_str, strlen(bpf_str), 1, f);
		if (fw_res < 1) {
			perror("fwrite");
		}
	}

	fprintf(f, "\t>>>>> EndOf generated eBPF code <<<<<<\n");
}
