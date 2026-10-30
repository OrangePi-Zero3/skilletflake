/* SPDX-License-Identifier: MIT */
/*
 * vk_e2e.c - end-to-end Vulkan flow on Mesa panvk -> libdrm-win -> the REAL kmd/core + mock G610
 * (libdrm-win transport "core", in-process), x86_64 under wine. No loader: the ICD is loaded
 * with LoadLibrary like tests in userspace/mesa/tests/icd_smoke.c.
 *
 *   vk_e2e.exe [path\to\vulkan_panfrost.dll]       (run with PANWIN_TRANSPORT=core)
 *
 * Steps (each one prints "STEP n <name>: PASS|FAIL ..."; the run stops at the first failure
 * that makes the rest meaningless, and the summary says how far it got):
 *   1 instance            vkCreateInstance (API 1.3)
 *   2 enumerate           exactly one physical device, name contains "Mali-G610" (or
 *                         $PANWIN_E2E_GPU: "Mali-G31" on the mock G31, PANWIN_CORE_MODEL=g31)
 *   3 device              vkCreateDevice with every queue of every family + timelineSemaphore
 *   4 memory              allocate / map / bind host-visible memory for two buffers
 *   5 record              vkCmdFillBuffer + vkCmdCopyBuffer + vkCmdSetEvent (CS-only)
 *   6 submit+fence        vkQueueSubmit with a fence, vkWaitForFences
 *   7 cs-data             vkGetEventStatus == VK_EVENT_SET: the command stream really ran on
 *                         the mock CSF executor (SYNC_SET32 by the "GPU" seen by the CPU)
 *   8 ts-query            vkCmdResetQueryPool + vkCmdWriteTimestamp2 (CS-only: STORE_STATE,
 *                         CS loops and linked-list updates): result available and non-zero
 *   9 sparse-bind         sparse buffer: vkQueueBindSparse bind + unbind on the bind-only
 *                         queue family (async VM_BIND with syncobj signalling), fenced
 *  10 timeline            submit waiting on timeline value 1 and signalling 2; host
 *                         vkSignalSemaphore(1) -> vkWaitSemaphores(2), counter == 2
 *  11 stress              N (default 1000, PANWIN_E2E_STRESS) resubmits of the step-5 command
 *                         buffer alternating over the queues of family 0, each signalling the
 *                         timeline: host vkWaitSemaphores + event SET/host-reset round trip;
 *                         wraps the kernel ring buffers several times
 *  12 every-queue         an empty submit + fence on every created queue
 *  13 idle               vkDeviceWaitIdle
 *  14 teardown           destroy everything, then the core transport reports no leaks
 * Steps for features the device doesn't advertise (8 without timestampValidBits, 9 without
 * sparseBinding: panvk v7 on the G31) print SKIP and count as passing.
 * Fill/copy results are printed for information only: panvk implements them with compute
 * shaders and the mock executes RUN_COMPUTE as a no-op.
 */
#define VK_NO_PROTOTYPES
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <vulkan/vulkan.h>

#include "panwin_core.h"

typedef VkResult(VKAPI_PTR *PFN_negotiate)(uint32_t *pVersion);

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
static int last_step, failed;

#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)

#define NSTEPS 14
static int nstep;

/* ok: 1 PASS, 0 FAIL, -1 SKIP (the device doesn't advertise the feature; counts as passing) */
static int step(const char *name, int ok, const char *fmt, ...)
{
	va_list ap;
	int n = ++nstep;

	printf("STEP %2d %-13s: %s", n, name, ok < 0 ? "SKIP" : ok ? "PASS" : "FAIL");
	if (fmt) {
		printf("  ");
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
	printf("\n");
	if (ok && !failed)
		last_step = n;
	if (!ok)
		failed = 1;
	return ok;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
	 const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
	(void)severity;
	(void)type;
	(void)user;
	printf("  [driver] %s\n", data->pMessage);
	return VK_FALSE;
}

static panwin_core_get_report_fn get_report;

static int core_report(struct panwin_core_report *r)
{
	memset(r, 0, sizeof(*r));
	r->version = PANWIN_CORE_REPORT_VERSION;
	return get_report && !get_report(r);
}

#define BUF_SIZE 65536u

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "vulkan_panfrost.dll";
	VkResult res;

	setvbuf(stdout, NULL, _IONBF, 0);
	HMODULE dll = LoadLibraryA(path);
	if (!dll) {
		printf("FAIL: LoadLibrary(%s) error %lu\n", path, GetLastError());
		return 1;
	}
	get_report = (panwin_core_get_report_fn)(void (*)(void))GetProcAddress(
		GetModuleHandleA("libdrm.dll"), PANWIN_CORE_GET_REPORT);
	if (!get_report)
		printf("note: libdrm.dll has no core transport report (not a -Dcore_transport build)\n");

	PFN_negotiate negotiate =
		(PFN_negotiate)(void *)GetProcAddress(dll, "vk_icdNegotiateLoaderICDInterfaceVersion");
	gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(dll, "vk_icdGetInstanceProcAddr");
	uint32_t iface = 7;
	if (!negotiate || !gipa || negotiate(&iface) != VK_SUCCESS) {
		printf("FAIL: ICD negotiation\n");
		return 1;
	}

	/* ---- 1 instance ---- */
	PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	const char *iext[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
	VkDebugUtilsMessengerCreateInfoEXT dbg = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
		.pfnUserCallback = debug_cb,
	};
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
				  .pApplicationName = "vk_e2e", .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &dbg,
				     .pApplicationInfo = &app, .enabledExtensionCount = 1,
				     .ppEnabledExtensionNames = iext };
	res = vkCreateInstance(&ici, NULL, &inst);
	if (!step("instance", res == VK_SUCCESS, "vkCreateInstance = %d", res))
		goto out;

	/* ---- 2 enumerate ---- */
	IFN(vkEnumeratePhysicalDevices);
	IFN(vkGetPhysicalDeviceProperties);
	IFN(vkGetPhysicalDeviceQueueFamilyProperties);
	IFN(vkGetPhysicalDeviceMemoryProperties);
	IFN(vkCreateDevice);
	IFN(vkGetPhysicalDeviceFeatures);
	IFN(vkDestroyInstance);
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	uint32_t npd = 0;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties props = { 0 };
	res = vkEnumeratePhysicalDevices(inst, &npd, NULL);
	if (npd >= 1) {
		VkPhysicalDevice pds[4];
		npd = npd > 4 ? 4 : npd;
		vkEnumeratePhysicalDevices(inst, &npd, pds);
		pd = pds[0];
		vkGetPhysicalDeviceProperties(pd, &props);
	}
	const char *want_gpu = getenv("PANWIN_E2E_GPU") ? getenv("PANWIN_E2E_GPU") : "Mali-G610";
	if (!step("enumerate", res == VK_SUCCESS && npd == 1 && strstr(props.deviceName, want_gpu),
		  "%u device(s): \"%s\", API %u.%u.%u, vendor 0x%x device 0x%x", npd,
		  props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
		  VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
		  props.vendorID, props.deviceID))
		goto out_inst;

	/* ---- 3 device with every queue ---- */
	uint32_t nqf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
	VkQueueFamilyProperties qfp[8];
	nqf = nqf > 8 ? 8 : nqf;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfp);
	VkDeviceQueueCreateInfo qci[8];
	float prios[8][16];
	uint32_t total_queues = 0;
	for (uint32_t i = 0; i < nqf; i++) {
		uint32_t n = qfp[i].queueCount > 16 ? 16 : qfp[i].queueCount;
		for (uint32_t j = 0; j < n; j++)
			prios[i][j] = 1.0f;
		qci[i] = (VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
						    .queueFamilyIndex = i, .queueCount = n,
						    .pQueuePriorities = prios[i] };
		printf("  queue family %u: flags 0x%x, %u queue(s)%s\n", i, qfp[i].queueFlags,
		       qfp[i].queueCount, n < qfp[i].queueCount ? " (capped at 16)" : "");
		total_queues += n;
	}
	VkPhysicalDeviceVulkan12Features f12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.timelineSemaphore = VK_TRUE,
	};
	VkPhysicalDeviceVulkan13Features f13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &f12,
		.synchronization2 = VK_TRUE,
	};
	VkPhysicalDeviceFeatures pdf = { 0 }, en = { 0 };
	vkGetPhysicalDeviceFeatures(pd, &pdf);
	int sparse = pdf.sparseBinding;
	en.sparseBinding = pdf.sparseBinding;
	en.sparseResidencyBuffer = pdf.sparseResidencyBuffer;
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
				   .queueCreateInfoCount = nqf, .pQueueCreateInfos = qci,
				   .pEnabledFeatures = &en };
	res = vkCreateDevice(pd, &dci, NULL, &dev);
	if (!step("device", res == VK_SUCCESS, "vkCreateDevice = %d, %u famil%s, %u queue(s)",
		  res, nqf, nqf == 1 ? "y" : "ies", total_queues))
		goto out_inst;

	DFN(vkGetDeviceQueue);
	DFN(vkDestroyDevice);
	DFN(vkCreateBuffer);
	DFN(vkDestroyBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkFreeMemory);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	DFN(vkUnmapMemory);
	DFN(vkCreateCommandPool);
	DFN(vkDestroyCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkCmdFillBuffer);
	DFN(vkCmdCopyBuffer);
	DFN(vkCmdPipelineBarrier);
	DFN(vkCmdSetEvent);
	DFN(vkCreateEvent);
	DFN(vkDestroyEvent);
	DFN(vkGetEventStatus);
	DFN(vkResetEvent);
	DFN(vkQueueBindSparse);
	DFN(vkCreateFence);
	DFN(vkDestroyFence);
	DFN(vkWaitForFences);
	DFN(vkResetFences);
	DFN(vkQueueSubmit);
	DFN(vkCreateSemaphore);
	DFN(vkDestroySemaphore);
	DFN(vkSignalSemaphore);
	DFN(vkWaitSemaphores);
	DFN(vkGetSemaphoreCounterValue);
	DFN(vkDeviceWaitIdle);
	DFN(vkCreateQueryPool);
	DFN(vkDestroyQueryPool);
	DFN(vkCmdResetQueryPool);
	DFN(vkCmdWriteTimestamp2);
	DFN(vkGetQueryPoolResults);

	/* ---- 4 memory ---- */
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	VkBuffer buf[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkDeviceMemory mem[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	uint32_t *map[2] = { NULL, NULL };
	int mem_ok = 1;
	for (int i = 0; i < 2 && mem_ok; i++) {
		VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					   .size = BUF_SIZE,
					   .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
						    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
						    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
		VkMemoryRequirements mr;
		uint32_t type = UINT32_MAX;
		mem_ok &= vkCreateBuffer(dev, &bci, NULL, &buf[i]) == VK_SUCCESS;
		if (!mem_ok)
			break;
		vkGetBufferMemoryRequirements(dev, buf[i], &mr);
		for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
			VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
						     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			if ((mr.memoryTypeBits & (1u << t)) &&
			    (mp.memoryTypes[t].propertyFlags & want) == want) {
				type = t;
				break;
			}
		}
		VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					     .allocationSize = mr.size, .memoryTypeIndex = type };
		mem_ok &= type != UINT32_MAX &&
			  vkAllocateMemory(dev, &mai, NULL, &mem[i]) == VK_SUCCESS &&
			  vkBindBufferMemory(dev, buf[i], mem[i], 0) == VK_SUCCESS &&
			  vkMapMemory(dev, mem[i], 0, VK_WHOLE_SIZE, 0, (void **)&map[i]) == VK_SUCCESS;
	}
	if (mem_ok) {
		for (uint32_t k = 0; k < BUF_SIZE / 4; k++) {
			map[0][k] = 0x10000000u + k;
			map[1][k] = 0;
		}
		mem_ok = map[0][123] == 0x10000000u + 123;
	}
	if (!step("memory", mem_ok, "2 x %u KiB host-visible+coherent, mapped at %p / %p",
		  BUF_SIZE / 1024, (void *)map[0], (void *)map[1]))
		goto out_dev;

	/* ---- 5 record ---- */
	VkQueue q0;
	vkGetDeviceQueue(dev, 0, 0, &q0);
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					 .queueFamilyIndex = 0 };
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cb[2];
	VkEvent event = VK_NULL_HANDLE;
	VkEventCreateInfo eci = { .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
	int rec_ok = vkCreateCommandPool(dev, &cpci, NULL, &pool) == VK_SUCCESS;
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					     .commandPool = pool,
					     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					     .commandBufferCount = 2 };
	rec_ok = rec_ok && vkAllocateCommandBuffers(dev, &cbai, cb) == VK_SUCCESS &&
		 vkCreateEvent(dev, &eci, NULL, &event) == VK_SUCCESS;
	if (rec_ok) {
		VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VkBufferCopy region = { .srcOffset = 0, .dstOffset = 4096, .size = 4096 };
		VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
				       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				       .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
							VK_ACCESS_TRANSFER_WRITE_BIT |
							VK_ACCESS_HOST_READ_BIT };
		rec_ok = vkBeginCommandBuffer(cb[0], &bi) == VK_SUCCESS;
		vkCmdFillBuffer(cb[0], buf[1], 0, 4096, 0xdeadbeefu);
		vkCmdPipelineBarrier(cb[0], VK_PIPELINE_STAGE_TRANSFER_BIT,
				     VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
				     1, &mb, 0, NULL, 0, NULL);
		vkCmdCopyBuffer(cb[0], buf[0], buf[1], 1, &region);
		vkCmdSetEvent(cb[0], event, VK_PIPELINE_STAGE_TRANSFER_BIT);
		rec_ok = rec_ok && vkEndCommandBuffer(cb[0]) == VK_SUCCESS;
		/* cb[1]: empty-ish, used by the timeline step */
		rec_ok = rec_ok && vkBeginCommandBuffer(cb[1], &bi) == VK_SUCCESS &&
			 vkEndCommandBuffer(cb[1]) == VK_SUCCESS;
	}
	if (!step("record", rec_ok, "fill + barrier + copy + set-event"))
		goto out_cmd;

	VkResult ev_before = event ? vkGetEventStatus(dev, event) : VK_ERROR_UNKNOWN;

	/* ---- 6 submit + fence ---- */
	VkFence fence = VK_NULL_HANDLE;
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
			    .pCommandBuffers = &cb[0] };
	VkResult sub = VK_ERROR_UNKNOWN, wait = VK_ERROR_UNKNOWN;
	if (vkCreateFence(dev, &fci, NULL, &fence) == VK_SUCCESS) {
		sub = vkQueueSubmit(q0, 1, &si, fence);
		if (sub == VK_SUCCESS)
			wait = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
	}
	if (!step("submit+fence", sub == VK_SUCCESS && wait == VK_SUCCESS,
		  "vkQueueSubmit = %d, vkWaitForFences = %d", sub, wait))
		goto out_cmd;

	/* ---- 7 CS-executed data ---- */
	VkResult evs = vkGetEventStatus(dev, event);
	step("cs-data", ev_before == VK_EVENT_RESET && evs == VK_EVENT_SET,
	     "vkGetEventStatus before submit = %d, after = %d (%s)", ev_before, evs,
	     evs == VK_EVENT_SET ? "SET" : evs == VK_EVENT_RESET ? "RESET" : "error");
	printf("  info: fill dst[0] = 0x%08x (0xdeadbeef if compute ran), copy dst[1024] = 0x%08x "
	       "(0x10000000 if compute ran): RUN_COMPUTE is a no-op in the mock\n",
	       map[1][0], map[1][1024]);

	/* ---- 8 timestamp query (CS-only; panvk v6/v7 have none: timestampValidBits 0) ---- */
	VkQueryPoolCreateInfo qpci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
				       .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
	VkQueryPool qp = VK_NULL_HANDLE;
	VkCommandBuffer cbq = VK_NULL_HANDLE;
	const int have_ts = qfp[0].timestampValidBits != 0;
	VkResult qres = have_ts ? vkCreateQueryPool(dev, &qpci, NULL, &qp) : VK_ERROR_FEATURE_NOT_PRESENT,
		 qsub = VK_ERROR_UNKNOWN, qget = VK_ERROR_UNKNOWN;
	uint64_t qr[4] = { 0 };
	cbai.commandBufferCount = 1;
	if (qres == VK_SUCCESS && vkAllocateCommandBuffers(dev, &cbai, &cbq) == VK_SUCCESS) {
		VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VkSubmitInfo sq = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
				    .pCommandBuffers = &cbq };
		vkBeginCommandBuffer(cbq, &bi);
		vkCmdResetQueryPool(cbq, qp, 0, 2);
		vkCmdWriteTimestamp2(cbq, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 0);
		vkEndCommandBuffer(cbq);
		vkResetFences(dev, 1, &fence);
		qsub = vkQueueSubmit(q0, 1, &sq, fence);
		if (qsub == VK_SUCCESS)
			qsub = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
		if (qsub == VK_SUCCESS)
			qget = vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(qr), qr, 2 * sizeof(uint64_t),
						     VK_QUERY_RESULT_64_BIT |
						     VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	}
	if (!have_ts)
		step("ts-query", -1, "timestampValidBits 0 on family 0");
	else
		step("ts-query", qsub == VK_SUCCESS && qget == VK_NOT_READY && qr[0] &&
				 qr[1] == 1 && qr[3] == 0,
		     "submit+wait %d, results %d (want NOT_READY=%d: query 1 never written), "
		     "q0 = %llu avail %llu, q1 avail %llu", qsub, qget, VK_NOT_READY,
		     (unsigned long long)qr[0], (unsigned long long)qr[1],
		     (unsigned long long)qr[3]);

	/* ---- 9 sparse binding on the bind queue ---- */
	{
		uint32_t bfam = UINT32_MAX;
		VkBuffer sbuf = VK_NULL_HANDLE;
		VkDeviceMemory smem = VK_NULL_HANDLE;
		VkResult b1 = VK_ERROR_FEATURE_NOT_PRESENT, b2 = VK_ERROR_FEATURE_NOT_PRESENT;
		VkMemoryRequirements smr = { 0 };

		for (uint32_t i = 0; i < nqf; i++)
			if (qfp[i].queueFlags == VK_QUEUE_SPARSE_BINDING_BIT)
				bfam = i;
		VkBufferCreateInfo sbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					    .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT,
					    .size = 4u << 20,
					    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
						     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
		if (sparse && bfam != UINT32_MAX &&
		    vkCreateBuffer(dev, &sbci, NULL, &sbuf) == VK_SUCCESS) {
			vkGetBufferMemoryRequirements(dev, sbuf, &smr);
			uint32_t type = 0;
			while (type < 32 && !(smr.memoryTypeBits & (1u << type)))
				type++;
			VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
						     .allocationSize = smr.size, .memoryTypeIndex = type };
			if (vkAllocateMemory(dev, &mai, NULL, &smem) == VK_SUCCESS) {
				VkQueue bq;
				VkSparseMemoryBind mbind = { .resourceOffset = 0, .size = smr.size,
							     .memory = smem };
				VkSparseBufferMemoryBindInfo bbi = { .buffer = sbuf, .bindCount = 1,
								     .pBinds = &mbind };
				VkBindSparseInfo bsi = { .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
							 .bufferBindCount = 1, .pBufferBinds = &bbi };

				vkGetDeviceQueue(dev, bfam, 0, &bq);
				vkResetFences(dev, 1, &fence);
				b1 = vkQueueBindSparse(bq, 1, &bsi, fence);
				if (b1 == VK_SUCCESS)
					b1 = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
				mbind.memory = VK_NULL_HANDLE; /* unbind */
				vkResetFences(dev, 1, &fence);
				b2 = vkQueueBindSparse(bq, 1, &bsi, fence);
				if (b2 == VK_SUCCESS)
					b2 = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
			}
		}
		if (!sparse)
			step("sparse-bind", -1, "sparseBinding not advertised");
		else
			step("sparse-bind", b1 == VK_SUCCESS && b2 == VK_SUCCESS,
			     "sparseBinding yes, bind family %d, %llu KiB: bind+fence %d, "
			     "unbind+fence %d", bfam == UINT32_MAX ? -1 : (int)bfam,
			     (unsigned long long)smr.size / 1024, b1, b2);
		if (sbuf)
			vkDestroyBuffer(dev, sbuf, NULL);
		if (smem)
			vkFreeMemory(dev, smem, NULL);
	}

	/* ---- 10 timeline semaphore host wait/signal ---- */
	VkSemaphoreTypeCreateInfo stci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
					   .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
					   .initialValue = 0 };
	VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &stci };
	VkSemaphore tl = VK_NULL_HANDLE;
	VkResult tres = vkCreateSemaphore(dev, &sci, NULL, &tl), tsub = VK_ERROR_UNKNOWN,
		 twait0 = VK_ERROR_UNKNOWN, tsig = VK_ERROR_UNKNOWN, twait = VK_ERROR_UNKNOWN;
	uint64_t counter = 0;
	if (tres == VK_SUCCESS) {
		uint64_t wv = 1, sv = 2;
		VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkTimelineSemaphoreSubmitInfo tsi = {
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &wv,
			.signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sv,
		};
		VkSubmitInfo si2 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi,
				     .waitSemaphoreCount = 1, .pWaitSemaphores = &tl,
				     .pWaitDstStageMask = &ws, .commandBufferCount = 1,
				     .pCommandBuffers = &cb[1], .signalSemaphoreCount = 1,
				     .pSignalSemaphores = &tl };
		VkSemaphoreWaitInfo swi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
					    .semaphoreCount = 1, .pSemaphores = &tl,
					    .pValues = &sv };
		VkSemaphoreSignalInfo ssi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
					      .semaphore = tl, .value = 1 };
		tsub = vkQueueSubmit(q0, 1, &si2, VK_NULL_HANDLE);
		/* Not signalled yet: the GPU work waits for the host. */
		twait0 = vkWaitSemaphores(dev, &swi, 20000000ull);
		tsig = vkSignalSemaphore(dev, &ssi);
		twait = vkWaitSemaphores(dev, &swi, 5000000000ull);
		vkGetSemaphoreCounterValue(dev, tl, &counter);
	}
	step("timeline", tres == VK_SUCCESS && tsub == VK_SUCCESS && twait0 == VK_TIMEOUT &&
			    tsig == VK_SUCCESS && twait == VK_SUCCESS && counter == 2,
	     "submit %d, early wait %d (want TIMEOUT=%d), host signal(1) %d, wait(2) %d, value %llu",
	     tsub, twait0, VK_TIMEOUT, tsig, twait, (unsigned long long)counter);

	/* ---- 11 stress ---- */
	char sv_env[16];
	DWORD svn = GetEnvironmentVariableA("PANWIN_E2E_STRESS", sv_env, sizeof(sv_env));
	uint32_t iters = (svn > 0 && svn < sizeof(sv_env)) ? (uint32_t)strtoul(sv_env, NULL, 0) : 1000;
	uint32_t done = 0, bad_ev = 0;
	VkResult sres = VK_SUCCESS;
	DWORD t0 = GetTickCount();
	for (uint32_t it = 0; it < iters && sres == VK_SUCCESS; it++) {
		VkQueue q;
		uint64_t sigv = 100 + it;
		VkTimelineSemaphoreSubmitInfo tsi = {
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sigv,
		};
		VkSubmitInfo ss = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi,
				    .commandBufferCount = 1, .pCommandBuffers = &cb[0],
				    .signalSemaphoreCount = 1, .pSignalSemaphores = &tl };
		VkSemaphoreWaitInfo swi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
					    .semaphoreCount = 1, .pSemaphores = &tl,
					    .pValues = &sigv };

		vkGetDeviceQueue(dev, 0, it % qci[0].queueCount, &q);
		vkResetEvent(dev, event);
		sres = vkQueueSubmit(q, 1, &ss, VK_NULL_HANDLE);
		if (sres == VK_SUCCESS)
			sres = vkWaitSemaphores(dev, &swi, 5000000000ull);
		if (sres == VK_SUCCESS) {
			bad_ev += vkGetEventStatus(dev, event) != VK_EVENT_SET;
			done++;
		}
	}
	step("stress", sres == VK_SUCCESS && done == iters && !bad_ev,
	     "%u/%u submits completed (last result %d), %u without the event set, %lu ms",
	     done, iters, sres, bad_ev, (unsigned long)(GetTickCount() - t0));

	/* ---- 12 every queue ---- */
	int allq_ok = 1;
	uint32_t nsub = 0;
	for (uint32_t i = 0; i < nqf; i++) {
		for (uint32_t j = 0; j < qci[i].queueCount; j++) {
			VkQueue q;
			VkSubmitInfo es = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
			VkResult a, b;
			vkGetDeviceQueue(dev, i, j, &q);
			vkResetFences(dev, 1, &fence);
			a = vkQueueSubmit(q, 1, &es, fence);
			b = a == VK_SUCCESS ? vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull) : a;
			if (a != VK_SUCCESS || b != VK_SUCCESS) {
				printf("  queue %u/%u: submit %d wait %d\n", i, j, a, b);
				allq_ok = 0;
			}
			nsub++;
		}
	}
	step("every-queue", allq_ok, "%u queue(s) submitted + fenced", nsub);

	/* ---- 10 idle ---- */
	res = vkDeviceWaitIdle(dev);
	step("idle", res == VK_SUCCESS, "vkDeviceWaitIdle = %d", res);

	struct panwin_core_report r;
	if (core_report(&r))
		printf("  core: CS executed %llu instrs, %llu RUN_*, %llu sync ops, %llu doorbells, "
		       "%u CS_FATAL (last type 0x%x); %llu DRM ioctls (%llu failed), %llu mmaps\n",
		       (unsigned long long)r.cs_instrs, (unsigned long long)r.cs_runs,
		       (unsigned long long)r.cs_syncs, (unsigned long long)r.cs_doorbells, r.cs_fatal,
		       r.cs_fatal_type, (unsigned long long)r.drm_ioctls,
		       (unsigned long long)r.drm_ioctl_errors, (unsigned long long)r.mmaps);

	vkDestroySemaphore(dev, tl, NULL);
	vkDestroyQueryPool(dev, qp, NULL);
	vkDestroyFence(dev, fence, NULL);
out_cmd:
	if (event)
		vkDestroyEvent(dev, event, NULL);
	if (pool)
		vkDestroyCommandPool(dev, pool, NULL);
out_dev:
	for (int i = 0; i < 2; i++) {
		if (map[i])
			vkUnmapMemory(dev, mem[i]);
		if (buf[i])
			vkDestroyBuffer(dev, buf[i], NULL);
		if (mem[i])
			vkFreeMemory(dev, mem[i], NULL);
	}
	vkDestroyDevice(dev, NULL);
out_inst:
	vkDestroyInstance(inst, NULL);
out:
	/* ---- 11 teardown: the last fd closed, so the core device must be gone, leak-free ---- */
	if (!failed || last_step >= 3) {
		struct panwin_core_report rr;

		nstep = NSTEPS - 1; /* teardown is always the last step */
		if (core_report(&rr)) {
			int clean = !rr.device_live && rr.open_files == 0 && !rr.leaked_allocs &&
				    !rr.leaked_mems && !rr.leaked_sync && !rr.model_errors &&
				    !rr.irq_unclaimed && !rr.leaked_maps && !rr.leaked_notifies &&
				    !rr.cs_fatal;
			step("teardown", clean,
			     "device live %u, open files %u, devices %u up/%u down; leaks: allocs "
			     "%lld mems %lld sync %lld maps %u events %u; mock model errors %u, "
			     "unclaimed irqs %u, CS_FATAL %u",
			     rr.device_live, rr.open_files, rr.devices_created, rr.devices_destroyed,
			     (long long)rr.leaked_allocs, (long long)rr.leaked_mems,
			     (long long)rr.leaked_sync, rr.leaked_maps, rr.leaked_notifies,
			     rr.model_errors, rr.irq_unclaimed, rr.cs_fatal);
		} else {
			step("teardown", 1, "(no core report: not the core transport)");
		}
	}
	FreeLibrary(dll);
	printf("RESULT: %s, last consecutive passing step %d of %d\n", failed ? "FAIL" : "PASS",
	       last_step, NSTEPS);
	return failed ? 1 : 0;
}
