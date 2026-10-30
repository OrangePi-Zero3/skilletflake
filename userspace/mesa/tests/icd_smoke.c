/* SPDX-License-Identifier: MIT */
/*
 * Loader-less smoke test for vulkan_panfrost.dll: loads the ICD with LoadLibrary, negotiates
 * the loader/ICD interface, lists instance extensions, creates an instance and enumerates
 * physical devices. Without panwin.sys (or libdrm-win's fake kernel) the device count is 0.
 *
 *   icd_smoke.exe [path\to\vulkan_panfrost.dll] [--require-device] [--create-device]
 *
 * --create-device also creates (and destroys) a VkDevice with one queue on device 0, which
 * exercises VM/BO/mmap/group/tiler-heap ioctls (useful with libdrm-win's fake kernel).
 *
 * Exit status: 0 on success, 1 on failure (VK_KHR_win32_surface missing, instance creation
 * failed, fd external handle types advertised, or --require-device and no device).
 */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <vulkan/vulkan.h>

typedef VkResult(VKAPI_PTR *PFN_negotiate)(uint32_t *pVersion);

/* Driver warnings/errors (vk_errorf) are only reported through debug messengers. */
static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
	 const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
	printf("  [driver] %s\n", data->pMessage);
	return VK_FALSE;
}

int
main(int argc, char **argv)
{
	const char *path = "vulkan_panfrost.dll";
	int require_device = 0, create_device = 0, ext_fd_bad = 0;
	VkResult dev_res = VK_SUCCESS;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--require-device"))
			require_device = 1;
		else if (!strcmp(argv[i], "--create-device"))
			create_device = 1;
		else
			path = argv[i];
	}

	HMODULE dll = LoadLibraryA(path);
	if (!dll) {
		printf("FAIL: LoadLibrary(%s) error %lu\n", path, GetLastError());
		return 1;
	}

	PFN_negotiate negotiate =
		(PFN_negotiate)(void *)GetProcAddress(dll, "vk_icdNegotiateLoaderICDInterfaceVersion");
	PFN_vkGetInstanceProcAddr gipa =
		(PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(dll, "vk_icdGetInstanceProcAddr");
	if (!negotiate || !gipa) {
		printf("FAIL: missing vk_icd* exports\n");
		return 1;
	}

	uint32_t version = 7;
	VkResult res = negotiate(&version);
	printf("vk_icdNegotiateLoaderICDInterfaceVersion: %d, interface version %u\n", res, version);
	if (res != VK_SUCCESS)
		return 1;

	PFN_vkEnumerateInstanceExtensionProperties enum_ext =
		(PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL,
			"vkEnumerateInstanceExtensionProperties");
	PFN_vkCreateInstance create_instance =
		(PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	if (!enum_ext || !create_instance) {
		printf("FAIL: global entrypoints missing\n");
		return 1;
	}

	uint32_t count = 0;
	enum_ext(NULL, &count, NULL);
	VkExtensionProperties *exts = calloc(count, sizeof(*exts));
	enum_ext(NULL, &count, exts);
	int have_win32 = 0, have_surface = 0, have_debug = 0;
	printf("instance extensions (%u):\n", count);
	for (uint32_t i = 0; i < count; i++) {
		printf("  %s (rev %u)\n", exts[i].extensionName, exts[i].specVersion);
		have_win32 |= !strcmp(exts[i].extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
		have_surface |= !strcmp(exts[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
		have_debug |= !strcmp(exts[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}
	printf("VK_KHR_win32_surface: %s\n", have_win32 ? "listed" : "MISSING");
	free(exts);

	const char *enable[3];
	uint32_t n_enable = 0;
	if (have_surface)
		enable[n_enable++] = VK_KHR_SURFACE_EXTENSION_NAME;
	if (have_win32)
		enable[n_enable++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
	if (have_debug)
		enable[n_enable++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

	VkDebugUtilsMessengerCreateInfoEXT dbg = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
		.pfnUserCallback = debug_cb,
	};

	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "icd_smoke",
		.apiVersion = VK_API_VERSION_1_4,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = have_debug ? &dbg : NULL,
		.pApplicationInfo = &app,
		.enabledExtensionCount = n_enable,
		.ppEnabledExtensionNames = enable,
	};
	VkInstance instance;
	res = create_instance(&ici, NULL, &instance);
	printf("vkCreateInstance: %d\n", res);
	if (res != VK_SUCCESS)
		return 1;

	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
		(PFN_vkCreateDebugUtilsMessengerEXT)gipa(instance, "vkCreateDebugUtilsMessengerEXT");
	PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
		(PFN_vkDestroyDebugUtilsMessengerEXT)gipa(instance, "vkDestroyDebugUtilsMessengerEXT");
	if (have_debug && create_messenger)
		create_messenger(instance, &dbg, NULL, &messenger);

	PFN_vkEnumeratePhysicalDevices enum_pd =
		(PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
	PFN_vkGetPhysicalDeviceProperties get_props =
		(PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
	PFN_vkDestroyInstance destroy_instance =
		(PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");

	uint32_t n_pd = 0;
	res = enum_pd(instance, &n_pd, NULL);
	printf("vkEnumeratePhysicalDevices: %d, %u device(s)\n", res, n_pd);
	if (n_pd) {
		VkPhysicalDevice *pds = calloc(n_pd, sizeof(*pds));
		enum_pd(instance, &n_pd, pds);
		for (uint32_t i = 0; i < n_pd; i++) {
			VkPhysicalDeviceProperties p;
			get_props(pds[i], &p);
			printf("  [%u] %s, Vulkan %u.%u.%u, vendor 0x%x device 0x%x\n", i, p.deviceName,
			       VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
			       VK_API_VERSION_PATCH(p.apiVersion), p.vendorID, p.deviceID);
		}
		/* There are no fd handles on Windows: no fd/dma-buf/sync-file handle type may be
		 * advertised through the core 1.1 external-handle queries. */
		PFN_vkGetPhysicalDeviceExternalSemaphoreProperties ext_sem =
			(PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)gipa(instance,
				"vkGetPhysicalDeviceExternalSemaphoreProperties");
		PFN_vkGetPhysicalDeviceExternalFenceProperties ext_fence =
			(PFN_vkGetPhysicalDeviceExternalFenceProperties)gipa(instance,
				"vkGetPhysicalDeviceExternalFenceProperties");
		PFN_vkGetPhysicalDeviceExternalBufferProperties ext_buf =
			(PFN_vkGetPhysicalDeviceExternalBufferProperties)gipa(instance,
				"vkGetPhysicalDeviceExternalBufferProperties");
		static const struct { uint32_t type; const char *name; } fd_types[] = {
			{ VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" },
			{ VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, "SYNC_FD" },
		};
		for (uint32_t t = 0; t < 2; t++) {
			VkPhysicalDeviceExternalSemaphoreInfo si = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
				.handleType = fd_types[t].type,
			};
			VkExternalSemaphoreProperties sp = {
				.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
			};
			ext_sem(pds[0], &si, &sp);
			VkPhysicalDeviceExternalFenceInfo fi = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
				.handleType = fd_types[t].type, /* same bit values for fences */
			};
			VkExternalFenceProperties fp = {
				.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES,
			};
			ext_fence(pds[0], &fi, &fp);
			printf("external %s: semaphore features 0x%x, fence features 0x%x\n",
			       fd_types[t].name, sp.externalSemaphoreFeatures, fp.externalFenceFeatures);
			ext_fd_bad |= sp.externalSemaphoreFeatures || fp.externalFenceFeatures;
		}
		VkPhysicalDeviceExternalBufferInfo bi = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
		};
		VkExternalBufferProperties bp = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
		};
		ext_buf(pds[0], &bi, &bp);
		printf("external OPAQUE_FD buffer: memory features 0x%x\n",
		       bp.externalMemoryProperties.externalMemoryFeatures);
		ext_fd_bad |= bp.externalMemoryProperties.externalMemoryFeatures != 0;
		if (ext_fd_bad)
			printf("FAIL: fd external handle types advertised on Windows\n");

		if (create_device) {
			PFN_vkCreateDevice cd = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
			float prio = 1.0f;
			VkDeviceQueueCreateInfo qci = {
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = 0,
				.queueCount = 1,
				.pQueuePriorities = &prio,
			};
			VkDeviceCreateInfo dci = {
				.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				.queueCreateInfoCount = 1,
				.pQueueCreateInfos = &qci,
			};
			VkDevice dev;
			dev_res = cd(pds[0], &dci, NULL, &dev);
			printf("vkCreateDevice: %d\n", dev_res);
			if (dev_res == VK_SUCCESS) {
				PFN_vkGetDeviceProcAddr gdpa =
					(PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
				PFN_vkDeviceWaitIdle wait_idle =
					(PFN_vkDeviceWaitIdle)gdpa(dev, "vkDeviceWaitIdle");
				PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice");
				printf("vkDeviceWaitIdle: %d\n", wait_idle(dev));
				dd(dev, NULL);
				printf("vkDestroyDevice: done\n");
			}
		}
		free(pds);
	}

	if (messenger)
		destroy_messenger(instance, messenger, NULL);
	destroy_instance(instance, NULL);
	FreeLibrary(dll);

	if (!have_win32 || (require_device && !n_pd) || dev_res != VK_SUCCESS || ext_fd_bad) {
		printf("FAIL\n");
		return 1;
	}
	printf("OK\n");
	return 0;
}
