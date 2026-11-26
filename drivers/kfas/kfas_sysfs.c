// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel Frame-Aware Scheduler (KFAS) - Sysfs Control & Telemetry Interface
 *
 * Exposes /sys/kernel/kfas/ for zero-reflash live tuning, Scene mode bridge,
 * userspace foreground watcher control, and real-time frame/thread observability.
 */
#include <linux/ctype.h>
#include <linux/kobject.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "kfas_internal.h"

static int kfas_cmp_u32(const void *a, const void *b)
{
	u32 va = *(const u32 *)a;
	u32 vb = *(const u32 *)b;

	return (va > vb) - (va < vb);
}

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(g_kfas.enabled) ? 1 : 0);
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	mutex_lock(&g_kfas.session_mutex);
	g_kfas.enabled = val;
	if (!val && g_kfas.session_active)
		kfas_session_stop_locked();
	mutex_unlock(&g_kfas.session_mutex);
	return count;
}

static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	enum kfas_mode m;

	mutex_lock(&g_kfas.ctrl_mutex);
	m = g_kfas.mode;
	mutex_unlock(&g_kfas.ctrl_mutex);
	return scnprintf(buf, PAGE_SIZE, "%s\n", kfas_mode_name(m));
}

static int kfas_parse_mode(const char *s, enum kfas_mode *out)
{
	if (sysfs_streq(s, "powersave") || sysfs_streq(s, "standby"))
		*out = KFAS_MODE_POWERSAVE;
	else if (sysfs_streq(s, "balance"))
		*out = KFAS_MODE_BALANCE;
	else if (sysfs_streq(s, "performance"))
		*out = KFAS_MODE_PERFORMANCE;
	else if (sysfs_streq(s, "fast") || sysfs_streq(s, "pedestal"))
		*out = KFAS_MODE_FAST;
	else
		return -EINVAL;
	return 0;
}

static ssize_t mode_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	enum kfas_mode m;

	if (kfas_parse_mode(buf, &m))
		return -EINVAL;

	mutex_lock(&g_kfas.ctrl_mutex);
	g_kfas.mode = m;
	mutex_unlock(&g_kfas.ctrl_mutex);
	return count;
}

static ssize_t target_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	ssize_t len = 0;
	u32 i;

	mutex_lock(&g_kfas.session_mutex);
	len += scnprintf(buf + len, PAGE_SIZE - len, "%d", g_kfas.target_pid);
	if (g_kfas.nr_cand_fps > 0) {
		len += scnprintf(buf + len, PAGE_SIZE - len, " ");
		for (i = 0; i < g_kfas.nr_cand_fps; i++) {
			len += scnprintf(buf + len, PAGE_SIZE - len, "%s%u",
					 i ? "," : "", g_kfas.cand_fps[i]);
		}
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	mutex_unlock(&g_kfas.session_mutex);
	return len;
}

/*
 * Format: "<pid> [fps1,fps2,...]"  or  "0" to detach.
 * Example: "12345 30,60,90,120"
 */
static ssize_t target_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	char tmp[128];
	char *p, *tok;
	int pid_val;
	u32 fps_list[KFAS_MAX_TARGET_FPS];
	u32 nr_fps = 0;
	int ret;

	if (count == 0 || count >= sizeof(tmp))
		return -EINVAL;

	memcpy(tmp, buf, count);
	tmp[count] = '\0';
	p = strim(tmp);

	tok = strsep(&p, " \t");
	if (!tok || kstrtoint(tok, 10, &pid_val) || pid_val < 0)
		return -EINVAL;

	if (pid_val == 0) {
		mutex_lock(&g_kfas.session_mutex);
		kfas_session_stop_locked();
		mutex_unlock(&g_kfas.session_mutex);
		return count;
	}

	if (p) {
		p = skip_spaces(p);
		while ((tok = strsep(&p, ",")) != NULL &&
		       nr_fps < KFAS_MAX_TARGET_FPS) {
			u32 f;

			if (*tok == '\0')
				continue;
			if (kstrtou32(tok, 10, &f) || f < 15 || f > 240)
				return -EINVAL;
			fps_list[nr_fps++] = f;
		}
	}

	if (nr_fps == 0) {
		fps_list[0] = 30;
		fps_list[1] = 60;
		fps_list[2] = 90;
		fps_list[3] = 120;
		nr_fps = 4;
	} else {
		u32 w = 0, r;

		sort(fps_list, nr_fps, sizeof(u32), kfas_cmp_u32, NULL);
		for (r = 0; r < nr_fps; r++) {
			if (w == 0 || fps_list[r] != fps_list[w - 1])
				fps_list[w++] = fps_list[r];
		}
		nr_fps = w;
	}

	mutex_lock(&g_kfas.session_mutex);
	ret = kfas_session_start_locked((pid_t)pid_val, fps_list, nr_fps);
	mutex_unlock(&g_kfas.session_mutex);
	return ret ? ret : (ssize_t)count;
}

static ssize_t probes_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	ssize_t len = 0;
	u32 i;

	mutex_lock(&g_kfas.session_mutex);
	len += scnprintf(buf + len, PAGE_SIZE - len, "%s ",
			 g_kfas.lib_path[0] ? g_kfas.lib_path : "-");
	if (g_kfas.nr_queue_offsets == 0) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "-");
	} else {
		for (i = 0; i < g_kfas.nr_queue_offsets; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len, "%s0x%llx",
					 i ? "," : "",
					 (unsigned long long)g_kfas.queue_offsets[i]);
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, " ");
	if (g_kfas.nr_dequeue_offsets == 0) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "-");
	} else {
		for (i = 0; i < g_kfas.nr_dequeue_offsets; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len, "%s0x%llx",
					 i ? "," : "",
					 (unsigned long long)g_kfas.dequeue_offsets[i]);
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	mutex_unlock(&g_kfas.session_mutex);
	return len;
}

static int kfas_parse_offsets(char *arg, loff_t *dst, u32 max_cnt, u32 *out_cnt)
{
	char *tok;
	u32 n = 0;

	*out_cnt = 0;
	if (!arg || strcmp(arg, "-") == 0)
		return 0;

	while ((tok = strsep(&arg, ",")) != NULL) {
		u64 v;

		if (*tok == '\0')
			continue;
		if (n >= max_cnt)
			return -EINVAL;
		if (kstrtou64(tok, 0, &v) || v == 0)
			return -EINVAL;
		dst[n++] = (loff_t)v;
	}
	*out_cnt = n;
	return 0;
}

/*
 * Format: "<lib_path> <q_off1[,q_off2]> [dq_off1[,dq_off2]|-]"
 * Example: "/system/lib64/libgui.so 0xf3fe4 0xf22d4"
 */
static ssize_t probes_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	char tmp[320];
	char *p, *path_tok, *q_tok, *dq_tok;
	loff_t q_offs[2], dq_offs[2];
	u32 nr_q = 0, nr_dq = 0;

	if (count == 0 || count >= sizeof(tmp))
		return -EINVAL;

	memcpy(tmp, buf, count);
	tmp[count] = '\0';
	p = strim(tmp);

	path_tok = strsep(&p, " \t");
	if (!path_tok || path_tok[0] != '/' ||
	    strlen(path_tok) >= KFAS_MAX_PATH_LEN)
		return -EINVAL;

	if (!p)
		return -EINVAL;
	p = skip_spaces(p);
	q_tok = strsep(&p, " \t");
	if (kfas_parse_offsets(q_tok, q_offs, 2, &nr_q) || nr_q == 0)
		return -EINVAL;

	if (p) {
		p = skip_spaces(p);
		dq_tok = strsep(&p, " \t");
		if (kfas_parse_offsets(dq_tok, dq_offs, 2, &nr_dq))
			return -EINVAL;
	}

	mutex_lock(&g_kfas.session_mutex);
	if (g_kfas.session_active)
		kfas_session_stop_locked();
	strscpy(g_kfas.lib_path, path_tok, sizeof(g_kfas.lib_path));
	memcpy(g_kfas.queue_offsets, q_offs, sizeof(q_offs));
	g_kfas.nr_queue_offsets = nr_q;
	memcpy(g_kfas.dequeue_offsets, dq_offs, sizeof(dq_offs));
	g_kfas.nr_dequeue_offsets = nr_dq;
	mutex_unlock(&g_kfas.session_mutex);
	return count;
}

static ssize_t placement_enable_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 READ_ONCE(g_kfas.placement_enable) ? 1 : 0);
}

static ssize_t placement_enable_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	mutex_lock(&g_kfas.ctrl_mutex);
	g_kfas.placement_enable = val;
	if (!val)
		kfas_place_idle_relax_locked();
	mutex_unlock(&g_kfas.ctrl_mutex);
	return count;
}

static ssize_t contain_workers_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 READ_ONCE(g_kfas.contain_workers) ? 1 : 0);
}

static ssize_t contain_workers_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	mutex_lock(&g_kfas.ctrl_mutex);
	g_kfas.contain_workers = val;
	mutex_unlock(&g_kfas.ctrl_mutex);
	return count;
}

static ssize_t unpause_big_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 READ_ONCE(g_kfas.unpause_big) ? 1 : 0);
}

static ssize_t unpause_big_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	mutex_lock(&g_kfas.ctrl_mutex);
	g_kfas.unpause_big = val;
	mutex_unlock(&g_kfas.ctrl_mutex);
	return count;
}

static ssize_t vip_comms_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	ssize_t len = 0;
	u32 i;

	mutex_lock(&g_kfas.ctrl_mutex);
	for (i = 0; i < g_kfas.nr_vip_comms; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				 i ? "," : "", g_kfas.vip_comms[i]);
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	mutex_unlock(&g_kfas.ctrl_mutex);
	return len;
}

static ssize_t vip_comms_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	char tmp[160];
	char *p, *tok;
	u32 n = 0;

	if (count >= sizeof(tmp))
		return -EINVAL;

	memcpy(tmp, buf, count);
	tmp[count] = '\0';
	p = strim(tmp);

	mutex_lock(&g_kfas.ctrl_mutex);
	memset(g_kfas.vip_comms, 0, sizeof(g_kfas.vip_comms));
	if (*p && strcmp(p, "-") != 0) {
		while ((tok = strsep(&p, ",")) != NULL &&
		       n < KFAS_MAX_VIP_COMMS) {
			tok = strim(tok);
			if (*tok)
				strscpy(g_kfas.vip_comms[n++], tok,
					KFAS_VIP_COMM_LEN);
		}
	}
	g_kfas.nr_vip_comms = n;
	mutex_unlock(&g_kfas.ctrl_mutex);
	return count;
}

static ssize_t tuning_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	ssize_t len = 0;
	int i;

	mutex_lock(&g_kfas.ctrl_mutex);
	for (i = 0; i < KFAS_MODE_COUNT; i++) {
		const struct kfas_mode_cfg *c = &g_kfas.mode_cfg[i];

		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "%s: margin_pct=%u up_gain=%u down_gain=%u down_hold=%u little_floor_khz=%u temp_mc=%u worker_umax=%u crit_umin=%u prime=%d\n",
				 kfas_mode_name((enum kfas_mode)i),
				 c->margin_pct, c->up_gain, c->down_gain,
				 c->down_hold_frames, c->little_floor_khz,
				 c->temp_thresh_mc, c->worker_uclamp_max,
				 c->crit_uclamp_min, c->prime_enable ? 1 : 0);
	}
	mutex_unlock(&g_kfas.ctrl_mutex);
	return len;
}

/* Format: "<mode> <param> <value>" */
static ssize_t tuning_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	char tmp[96];
	char *p, *m_tok, *k_tok, *v_tok;
	enum kfas_mode m;
	struct kfas_mode_cfg *c;
	u32 val;

	if (count == 0 || count >= sizeof(tmp))
		return -EINVAL;

	memcpy(tmp, buf, count);
	tmp[count] = '\0';
	p = strim(tmp);

	m_tok = strsep(&p, " \t");
	if (!m_tok || !p || kfas_parse_mode(m_tok, &m))
		return -EINVAL;
	p = skip_spaces(p);
	k_tok = strsep(&p, " \t");
	if (!k_tok || !p)
		return -EINVAL;
	p = skip_spaces(p);
	v_tok = strsep(&p, " \t");
	if (!v_tok || kstrtou32(v_tok, 10, &val))
		return -EINVAL;

	mutex_lock(&g_kfas.ctrl_mutex);
	c = &g_kfas.mode_cfg[m];
	if (strcmp(k_tok, "margin_pct") == 0 && val <= 35)
		c->margin_pct = val;
	else if (strcmp(k_tok, "up_gain") == 0 && val >= 20 && val <= 400)
		c->up_gain = val;
	else if (strcmp(k_tok, "down_gain") == 0 && val >= 10 && val <= 300)
		c->down_gain = val;
	else if (strcmp(k_tok, "down_hold") == 0 && val >= 2 && val <= 120)
		c->down_hold_frames = val;
	else if (strcmp(k_tok, "little_floor_khz") == 0)
		c->little_floor_khz = val;
	else if (strcmp(k_tok, "temp_mc") == 0)
		c->temp_thresh_mc = val;
	else if (strcmp(k_tok, "worker_umax") == 0 && val <= 1024)
		c->worker_uclamp_max = val;
	else if (strcmp(k_tok, "crit_umin") == 0 && val <= 1024)
		c->crit_uclamp_min = val;
	else if (strcmp(k_tok, "prime") == 0 && val <= 1)
		c->prime_enable = !!val;
	else {
		mutex_unlock(&g_kfas.ctrl_mutex);
		return -EINVAL;
	}
	mutex_unlock(&g_kfas.ctrl_mutex);
	return count;
}

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	struct kfas_gpu_snap gpu;
	ssize_t len = 0;
	u32 i;

	kfas_gpu_get_snapshot(&gpu);

	mutex_lock(&g_kfas.ctrl_mutex);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "version=%s enabled=%d active=%d rendering=%d mode=%s\n",
			 KFAS_VERSION, g_kfas.enabled ? 1 : 0,
			 g_kfas.session_active ? 1 : 0,
			 g_kfas.rendering_active ? 1 : 0,
			 kfas_mode_name(g_kfas.mode));
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "pid=%d render_tid=%d target_fps=%u cur_fps=%u.%u perf=%u/1000 bn=%s\n",
			 READ_ONCE(g_kfas.target_pid), g_kfas.render_tid,
			 g_kfas.locked_fps, g_kfas.cur_fps_x10 / 10,
			 g_kfas.cur_fps_x10 % 10, g_kfas.perf_level,
			 kfas_bn_name(g_kfas.last_bn));
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "ema_us: frame=%u exec=%u delay=%u dq_wait=%u frames=%llu dropped=%llu\n",
			 g_kfas.ema_frame_us, g_kfas.ema_exec_us,
			 g_kfas.ema_delay_us, g_kfas.ema_dq_wait_us,
			 (unsigned long long)g_kfas.frames_total,
			 (unsigned long long)g_kfas.frames_dropped_ring);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "gpu: valid=%d busy=%u%% freq=%u/%uMHz cpu_temp=%umC core_ctl_boost=%d\n",
			 gpu.valid ? 1 : 0, gpu.busy_pct, gpu.freq_mhz,
			 gpu.max_freq_mhz, g_kfas.last_cpu_temp_mc,
			 g_kfas.core_ctl_boosted ? 1 : 0);
	for (i = 0; i < g_kfas.nr_clusters; i++) {
		const struct kfas_cluster *cl = &g_kfas.clusters[i];

		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "cluster%u(policy%u cap=%lu): min=%u max=%u\n",
				 i, cl->policy_cpu, cl->cap_orig,
				 cl->cur_min_khz,
				 cl->cur_max_khz == FREQ_QOS_MAX_DEFAULT_VALUE ?
					0 : cl->cur_max_khz);
	}
	mutex_unlock(&g_kfas.ctrl_mutex);
	return len;
}

static ssize_t threads_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return kfas_place_format_threads(buf, PAGE_SIZE);
}

static struct kobj_attribute kfas_attr_enable = __ATTR_RW(enable);
static struct kobj_attribute kfas_attr_mode = __ATTR_RW(mode);
static struct kobj_attribute kfas_attr_target = __ATTR_RW(target);
static struct kobj_attribute kfas_attr_probes = __ATTR_RW(probes);
static struct kobj_attribute kfas_attr_placement_enable =
	__ATTR_RW(placement_enable);
static struct kobj_attribute kfas_attr_contain_workers =
	__ATTR_RW(contain_workers);
static struct kobj_attribute kfas_attr_unpause_big = __ATTR_RW(unpause_big);
static struct kobj_attribute kfas_attr_vip_comms = __ATTR_RW(vip_comms);
static struct kobj_attribute kfas_attr_tuning = __ATTR_RW(tuning);
static struct kobj_attribute kfas_attr_status = __ATTR_RO(status);
static struct kobj_attribute kfas_attr_threads = __ATTR_RO(threads);

static struct attribute *kfas_attrs[] = {
	&kfas_attr_enable.attr,
	&kfas_attr_mode.attr,
	&kfas_attr_target.attr,
	&kfas_attr_probes.attr,
	&kfas_attr_placement_enable.attr,
	&kfas_attr_contain_workers.attr,
	&kfas_attr_unpause_big.attr,
	&kfas_attr_vip_comms.attr,
	&kfas_attr_tuning.attr,
	&kfas_attr_status.attr,
	&kfas_attr_threads.attr,
	NULL,
};

static const struct attribute_group kfas_attr_group = {
	.attrs = kfas_attrs,
};

int kfas_sysfs_init(void)
{
	int ret;

	g_kfas.kobj = kobject_create_and_add("kfas", kernel_kobj);
	if (!g_kfas.kobj)
		return -ENOMEM;

	ret = sysfs_create_group(g_kfas.kobj, &kfas_attr_group);
	if (ret) {
		kobject_put(g_kfas.kobj);
		g_kfas.kobj = NULL;
	}
	return ret;
}

void kfas_sysfs_exit(void)
{
	if (!g_kfas.kobj)
		return;
	sysfs_remove_group(g_kfas.kobj, &kfas_attr_group);
	kobject_put(g_kfas.kobj);
	g_kfas.kobj = NULL;
}
