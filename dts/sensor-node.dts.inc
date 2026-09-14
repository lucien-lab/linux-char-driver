	virt_i2c: virt-i2c {
		compatible = "lucien,virt-i2c";
		#address-cells = <0x01>;
		#size-cells = <0x00>;
		status = "okay";

		sensor0: sensor@48 {
			compatible = "lucien,sensor-char";
			reg = <0x48>;
			poll-interval-ms = <0x1f4>;
		};

		/*
		 * 第二颗同型号芯片（从机地址 0x49），由 IIO 驱动绑定。
		 * virt_i2c 为每个从机地址维护独立 bank（0x48~0x4b），
		 * 所以字符设备驱动与 IIO 驱动可以同时装载、互不干扰。
		 * 注意：IIO 阶段只加载 virt_i2c + sensor_iio（见 tests/phases/05-iio.modules）
		 */
		sensor1: sensor@49 {
			compatible = "lucien,sensor-iio";
			reg = <0x49>;
		};
	};
