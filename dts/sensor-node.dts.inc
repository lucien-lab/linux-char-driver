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
	};
