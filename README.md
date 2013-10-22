
STEP1) Modify /etc/libvirt/qemu.conf:
	#cgroup_controllers = [ "cpu", "devices", "memory", "blkio", "cpuset", "cpuacct" ]
	cgroup_controllers = [ "cpu", "memory", "blkio", "cpuset", "cpuacct" ]
	service libvirtd restart

STEP2) enable virtio-dataplane
	@hw/virtio/Makefile.objs
	-- common-obj-$(CONFIG_VIRTIO_BLK_DATA_PLANE) += dataplane/
	++ common-obj-y += dataplane/
