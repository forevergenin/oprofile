/**
 * @file daemon/opd_kernel.c
 * Dealing with the kernel and kernel module samples
 *
 * @remark Copyright 2002 OProfile authors
 * @remark Read the file COPYING
 *
 * @author John Levon
 * @author Philippe Elie
 */

#include "opd_kernel.h"
#include "opd_image.h"
#include "opd_printf.h"
#include "opd_stats.h"

#include "op_fileio.h"
#include "op_config_25.h"
#include "op_libiberty.h"

#include "p_module.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* kernel module */
struct opd_module {
	char * name;
	struct opd_image * image;
	vma_t start;
	vma_t end;
	/* used when separate_kernel_samples != 0 */
	struct list_head module_list;
};

extern char * vmlinux;
extern int verbose;
extern unsigned long opd_stats[];

static struct opd_image * kernel_image;

/* kernel and module support */
static vma_t kernel_start;
static vma_t kernel_end;
static struct opd_module opd_modules[OPD_MAX_MODULES];
static unsigned int nr_modules=0;

/**
 * opd_init_kernel_image - initialise the kernel image
 */
void opd_init_kernel_image(void)
{
	kernel_image = opd_get_kernel_image(vmlinux);
}

/**
 * opd_parse_kernel_range - parse the kernel range values
 */
void opd_parse_kernel_range(char const * arg)
{
	sscanf(arg, "%llx,%llx", &kernel_start, &kernel_end);

	verbprintf("OPD_PARSE_KERNEL_RANGE: kernel_start = %llx, kernel_end = %llx\n",
		   kernel_start, kernel_end);

	if (kernel_start == 0x0 || kernel_end == 0x0) {
		fprintf(stderr,
			"Warning: mis-parsed kernel range: %llx-%llx\n",
			kernel_start, kernel_end);
		fprintf(stderr, "kernel profiles will be wrong.\n");
	}
}

/**
 * opd_clear_module_info - clear kernel module information
 *
 * Clear and free all kernel module information and reset
 * values.
 */
void opd_clear_module_info(void)
{
	int i;

	for (i=0; i < OPD_MAX_MODULES; i++) {
		if (opd_modules[i].name)
			free(opd_modules[i].name);
		opd_modules[i].name = NULL;
		opd_modules[i].start = 0;
		opd_modules[i].end = 0;
		list_init(&opd_modules[i].module_list);
	}
}

/**
 * new_module - initialise a module description
 *
 * @param name module name
 * @param start start address
 * @param end end address
 */
static struct opd_module * new_module(char * name, vma_t start, vma_t end)
{ 
	opd_modules[nr_modules].name = name;
	opd_modules[nr_modules].image = NULL;
	opd_modules[nr_modules].start = start;
	opd_modules[nr_modules].end = end;
	list_init(&opd_modules[nr_modules].module_list);
	nr_modules++;
	if (nr_modules == OPD_MAX_MODULES) {
		fprintf(stderr, "Exceeded %u kernel modules !\n", OPD_MAX_MODULES);
		exit(EXIT_FAILURE);
	}
	return &opd_modules[nr_modules-1];
}

/**
 * opd_create_module - allocate and initialise a module description
 * @param name module name
 * @param start start address
 * @param end end address
 */
static struct opd_module * opd_create_module(char const * name,
				vma_t start, vma_t end)
{
	struct opd_module * module = xmalloc(sizeof(struct opd_module));

	module->name = xstrdup(name);
	module->image = NULL;
	module->start = start;
	module->end = end;
	list_init(&module->module_list);

	return module;
}

/**
 * opd_get_module - get module structure
 * @param name  name of module image
 *
 * Find the module structure for module image name.
 * If it could not be found, add the module to
 * the global module structure.
 *
 * If an existing module is found, name is free()d.
 * Otherwise it must be freed when the module structure
 * is removed (i.e. in opd_clear_module_info()).
 */
static struct opd_module * opd_get_module(char * name)
{
	int i;

	for (i=0; i < OPD_MAX_MODULES; i++) {
		if (opd_modules[i].name && !strcmp(name, opd_modules[i].name)) {
			/* free this copy */
			free(name);
			return &opd_modules[i];
		}
	}

	return new_module(name, 0, 0);
}


/**
 * opd_read_module_info - parse /proc/modules for kernel modules
 *
 * Parse the file /proc/module to read start address and size of module
 * each line is in the format:
 *
 * module_name 16480 1 dependencies Live 0xe091e000
 *
 * without any blank space in each field
 *
 */
static void opd_read_module_info(void)
{
	FILE * fp;
	char * line;
	struct opd_module * mod;
	int module_size;
	int ref_count;
	int ret;
	char module_name[256+1];
	char live_info[32];
	char dependencies[4096];
	unsigned long long start_address;

	fp = op_try_open_file("/proc/modules", "r");

	if (!fp) {
		printf("oprofiled: /proc/modules not readable, can't process module samples.\n");
		return;
	}

	while (1) {
		line = op_get_line(fp);

		if (feof(fp)) {
			free(line);
			break;
		} else if (line[0] == '\0') {
			free(line);
			continue;
		}

		/* Hacky: pp tools rely on at least one '}' in samples filename
		 * so I force module filename as if it reside in / directory */
		module_name[0] = '/';

		ret = sscanf(line, "%256s %u %u %4096s %32s %llx",
			     module_name+1, &module_size, &ref_count,
			     dependencies, live_info, &start_address);
		if (ret != 6) {
			printf("bad /proc/modules entry: %s\n", line);
			free(line);
			continue;
		}

		mod = opd_get_module(xstrdup(module_name));
		mod->image = opd_get_kernel_image(module_name);

		mod->start = start_address;
		mod->end = mod->start + module_size;

		verbprintf("module %s start %llx end %llx\n",
			   mod->name, mod->start, mod->end);

		free(line);
	}

	op_close_file(fp);
}


/**
 * opd_get_module_info - parse mapping information for kernel modules
 *
 * Parse the file /proc/ksyms to read in mapping information for
 * all kernel modules. The modutils package adds special symbols
 * to this file which allows determination of the module image
 * and mapping addresses of the form :
 *
 * __insmod_modulename_Oobjectfile_Mmtime_Vversion
 * __insmod_modulename_Ssectionname_Llength
 *
 * Currently the image file "objectfile" is stored, and details of
 * ".text" sections.
 *
 * There is no query_module API that allow to get directly the pathname
 * of a module so we need to parse all the /proc/ksyms.
 *
 * FIXME remove this function when we drop old modules compatibility
 */
static void opd_get_module_info(void)
{
	char * line;
	char * cp, * cp2, * cp3;
	FILE * fp;
	struct opd_module * mod;
	char * modname;
	char * filename;

	nr_modules=0;

	fp = op_try_open_file("/proc/ksyms", "r");

	if (!fp) {
		printf("oprofiled: /proc/ksyms not readable, fallback to /proc/modules.\n");
		opd_read_module_info();
		return;
	}

	while (1) {
		line = op_get_line(fp);
		if (!strcmp("", line) && !feof(fp)) {
			free(line);
			continue;
		} else if (!strcmp("",line))
			goto failure;

		if (strlen(line) < 9) {
			printf("oprofiled: corrupt /proc/ksyms line \"%s\"\n", line);
			goto failure;
		}

		if (strncmp("__insmod_", line + 9, 9)) {
			free(line);
			continue;
		}

		cp = line + 18;
		cp2 = cp;
		while ((*cp2) && !!strncmp("_S", cp2 + 1, 2) && !!strncmp("_O", cp2 + 1, 2))
			cp2++;

		if (!*cp2) {
			printf("oprofiled: corrupt /proc/ksyms line \"%s\"\n", line);
			goto failure;
		}

		cp2++;
		/* freed by opd_clear_module_info() or opd_get_module() */
		modname = xmalloc((size_t)((cp2 - cp) + 1));
		strncpy(modname, cp, (size_t)((cp2 - cp)));
		modname[cp2-cp] = '\0';

		mod = opd_get_module(modname);

		switch (*(++cp2)) {
			case 'O':
				/* get filename */
				cp2++;
				cp3 = cp2;

				while ((*cp3) && !!strncmp("_M", cp3 + 1, 2))
					cp3++;

				if (!*cp3) {
					free(line);
					continue;
				}

				cp3++;
				filename = xmalloc((size_t)(cp3 - cp2 + 1));
				strncpy(filename, cp2, (size_t)(cp3 - cp2));
				filename[cp3-cp2] = '\0';

				mod->image = opd_get_kernel_image(filename);
				free(filename);
				break;

			case 'S':
				/* get extent of .text section */
				cp2++;
				if (strncmp(".text_L", cp2, 7)) {
					free(line);
					continue;
				}

				cp2 += 7;
				sscanf(line,"%llx", &mod->start);
				sscanf(cp2,"%llu", &mod->end);
				mod->end += mod->start;
				break;
		}

		free(line);
	}

failure:
	free(line);
	op_close_file(fp);
}
 

/**
 * opd_drop_module_sample - drop a module sample efficiently
 * @param eip  eip of sample
 */
static void opd_drop_module_sample(vma_t eip)
{
	char * module_names;
	char * name;
	size_t size = 1024;
	size_t ret;
	uint nr_mods;
	uint mod = 0;

	module_names = xmalloc(size);
	while (query_module(NULL, QM_MODULES, module_names, size, &ret)) {
		if (errno != ENOSPC) {
			verbprintf("query_module failed: %s\n", strerror(errno));
			return;
		}
		size = ret;
		module_names = xrealloc(module_names, size);
	}

	nr_mods = ret;
	name = module_names;

	while (mod < nr_mods) {
		struct module_info info;
		if (!query_module(name, QM_INFO, &info, sizeof(info), &ret)) {
			if (eip >= info.addr && eip < info.addr + info.size) {
				verbprintf("Sample from unprofilable module %s\n", name);
				new_module(xstrdup(name), info.addr, info.addr + info.size);
				goto out;
			}
		}
		mod++;
		name += strlen(name) + 1;
	}
out:
	if (module_names)
		free(module_names);
}


/**
 * opd_find_module_by_eip - find a module by its eip
 * @param eip  EIP value
 *
 * find in the modules container the module which
 * contain this eip return %NULL if not found.
 * caller must check than the module image is valid
 */
static struct opd_module * opd_find_module_by_eip(vma_t eip)
{
	uint i;
	for (i = 0; i < nr_modules; i++) {
		if (opd_modules[i].start && opd_modules[i].end &&
		    opd_modules[i].start <= eip && opd_modules[i].end > eip)
			return &opd_modules[i];
	}

	return NULL;
}


/**
 * opd_handle_module_sample - process a module sample
 * @param eip  EIP value
 * @param counter  counter number
 *
 * Process a sample in module address space. The sample eip
 * is matched against module information. If the search was
 * successful, the sample is output to the relevant file.
 *
 * Note that for modules and the kernel, the offset will be
 * wrong in the file, as it is not a file offset, but the offset
 * from the text section. This is fixed up in pp.
 *
 * If the sample could not be located in a module, it is treated
 * as a kernel sample. This function is never called when
 * separate_kernel_samples != 0
 */
static void opd_handle_module_sample(vma_t eip, u32 counter)
{
	struct opd_module * module;

	module = opd_find_module_by_eip(eip);
	if (!module) {
		/* not found in known modules, re-read our info and retry */
		opd_clear_module_info();
		opd_get_module_info();

		module = opd_find_module_by_eip(eip);
	}

	if (module) {
		if (module->image != NULL) {
			opd_stats[OPD_MODULE]++;
			opd_put_image_sample(module->image,
					     eip - module->start, counter);
		} else {
			opd_stats[OPD_LOST_MODULE]++;
			verbprintf("No image for sampled module %s\n",
				   module->name);
		}
	} else {
		/* ok, we failed to place the sample even after re-reading
		 * /proc/ksyms. It's either a rogue sample, or from a module
		 * that didn't create symbols (like in some initrd setups).
		 * So we check with query_module() if we can place it in a
		 * symbol-less module, and if so create a negative entry for
		 * it, to quickly ignore future samples.
		 *
		 * Problem uncovered by Bob Montgomery <bobm@fc.hp.com>
		 */
		opd_stats[OPD_LOST_MODULE]++;
		opd_drop_module_sample(eip);
	}
}

static struct opd_module * opd_find_module(struct opd_image const * app_image,
					   vma_t eip)
{
	struct list_head * pos;
	struct opd_module * module;
 
	list_for_each(pos, &app_image->module_list) {
		module = list_entry(pos, struct opd_module, module_list);

		if (module->start && module->end &&
		    module->start <= eip && module->end > eip) {
			return module;
		}
	}

	return 0;
}


/**
 * opd_setup_kernel_sample
 * @param eip  EIP value of sample
 * @param counter  counter number
 * @param app_image  owning application of this sample
 * @param name  module name
 * @param start  module start address
 * @param end  module end address
 *
 * create an opd_module and associated opd_image then put a kernel module
 * sample in the newly created image
 */
static void opd_setup_kernel_sample(vma_t eip, u32 counter,
				    struct opd_image * app_image,
				    char const * name,
				    vma_t start, vma_t end)
{
	struct opd_image * image;
	struct opd_module * new_module;

	image = opd_add_kernel_image(name, app_image->app_name);
	if (!image) {
		verbprintf("Can't create image for %s %s\n",
			   name, app_image->app_name);
		return;
	}

	new_module = opd_create_module(name, start, end);
	new_module->image = image;
	list_add(&new_module->module_list, &app_image->module_list);

	verbprintf("Image (%s) offset 0x%llx, counter %u\n",
		   new_module->image->name, eip, counter);
	opd_put_image_sample(image, eip - new_module->start, counter);
}

/**
 * opd_put_kernel_sample - process a kernel sample when
 *   separate_kernel_samples != 0
 * @param eip  EIP value of sample
 * @param counter  counter number
 * @param app_image  owning application of this sample
 *
 * Handle a sample in kernel address space or in a module. The sample is
 * output to the relevant image file.
 */
static void opd_put_kernel_sample(vma_t eip, u32 counter,
				  struct opd_image * app_image)
{
	struct opd_module * module = opd_find_module(app_image, eip);
	if (module) {
		verbprintf("Image (%s) offset 0x%Lx, counter %u\n",
			   module->image->name, eip, counter);
		opd_put_image_sample(module->image, eip - module->start,
				     counter);
		return;
	}

	if (eip >= kernel_start && eip < kernel_end) {
		opd_setup_kernel_sample(eip, counter, app_image, vmlinux,
					kernel_start, kernel_end);
		return;
	}

	module = opd_find_module_by_eip(eip);
	if (!module) {
		/* not found in known modules, re-read and retry */
		opd_clear_module_info();
		opd_get_module_info();

		/* FIXME: test this */
		opd_for_each_image(opd_delete_modules);

		module = opd_find_module_by_eip(eip);
	}

	if (module) {
		/* fix bad parsing /proc/ksyms see opd_get_module_info().
		 * We can get also nil image for initrd setups:
		 * opd_drop_module_sample() create a module but can't create
		 * a proper image */
		if (!module->image) {
			opd_stats[OPD_LOST_MODULE]++;
			verbprintf("module %s : nil image\n", module->name);
			return;
		}

		if (!module->image->name) {
			opd_stats[OPD_LOST_MODULE]++;
			verbprintf("unable to get path name for module %s\n",
				   module->name);
			return;
		}
		opd_setup_kernel_sample(eip, counter, app_image,
					module->image->name,
					module->start, module->end);
	} else {
		/* ok, we failed to place the sample even after re-reading
		 * /proc/ksyms. It's either a rogue sample, or from a module
		 * that didn't create symbols (like in some initrd setups).
		 * So we check with query_module() if we can place it in a
		 * symbol-less module, and if so create a negative entry for
		 * it, to quickly ignore future samples.
		 *
		 * Problem uncovered by Bob Montgomery <bobm@fc.hp.com>
		 */
		opd_stats[OPD_LOST_MODULE]++;
		opd_drop_module_sample(eip);
	}
}

/**
 * opd_handle_kernel_sample - process a kernel sample
 * @param eip  EIP value of sample
 * @param counter  counter number
 * @param app_image  owning application of this sample or 0 when
 *   !separate_kernel_samples
 *
 * Handle a sample in kernel address space or in a module. The sample is
 * output to the relevant image file.
 */
void opd_handle_kernel_sample(vma_t eip, u32 counter, 
			      struct opd_image * app_image)
{
	if (!app_image) {
		if (eip >= kernel_start && eip < kernel_end) {
			opd_stats[OPD_KERNEL]++;
			opd_put_image_sample(kernel_image, eip - kernel_start, counter);
			return;
		}

		/* in a module */
		opd_handle_module_sample(eip, counter);
	} else {
		opd_put_kernel_sample(eip, counter, app_image);
	}
}
 
/**
 * opd_eip_is_kernel - is the sample from kernel/module space
 * @param eip  EIP value
 *
 * Returns %1 if eip is in the address space starting at
 * kernel_start, %0 otherwise.
 */
int opd_eip_is_kernel(vma_t eip)
{
	return (eip >= kernel_start);
}

void opd_delete_modules(struct opd_image * image)
{
	struct list_head * pos;
	struct list_head * pos2;
	struct opd_module * module;

	verbprintf("Removing image module list for image %p\n", image);
	list_for_each_safe(pos, pos2, &image->module_list) {
		module = list_entry(pos, struct opd_module, module_list);
		if (module->name)
			free(module->name);
		free(module);
	}

	list_init(&image->module_list);
}
