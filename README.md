
STEP1) Modify /etc/libvirt/qemu.conf:
	#cgroup_controllers = [ "cpu", "devices", "memory", "blkio", "cpuset", "cpuacct" ]
	cgroup_controllers = [ "cpu", "memory", "blkio", "cpuset", "cpuacct" ]
	service libvirtd restart
