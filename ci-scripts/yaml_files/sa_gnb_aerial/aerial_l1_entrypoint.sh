#!/bin/bash

# Check if cuBB_SDK is defined, if not, use default path
cuBB_Path="${cuBB_SDK:-/opt/nvidia/cuBB}"


cd "$cuBB_Path" || exit 1
# Add gdrcopy to LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/local/nvidia/lib:/usr/local/nvidia/lib64:/opt/mellanox/dpdk/lib/x86_64-linux-gnu:/opt/mellanox/doca/lib/x86_64-linux-gnu:/opt/nvidia/cuBB/cuPHY-CP/external/gdrcopy/build/x86_64/

# Restart MPS
# Export variables
export CUDA_DEVICE_MAX_CONNECTIONS=8
export CUDA_MPS_PIPE_DIRECTORY=/var
export CUDA_MPS_LOG_DIRECTORY=/var

# Stop existing MPS
echo quit | nvidia-cuda-mps-control

# Start MPS
sudo -E nvidia-cuda-mps-control -d
sudo -E echo start_server -uid 0 | sudo -E nvidia-cuda-mps-control

# Start cuphycontroller_scf
# Check if an argument is provided
if [ $# -eq 0 ]; then
    # No argument provided, use default value
    argument="P5G_SCF_FXN"
else
    # Argument provided, use it
    argument="$1"
fi

#sed -i "s/ nic:.*/ nic: 0000:cc:00.1/" ${cuBB_SDK}/cuPHY-CP/cuphycontroller/config/cuphycontroller_P5G_FXN_R750.yaml
#sed -i "s/ dst_mac_addr:.*/ dst_mac_addr: 6c:ad:ad:00:04:6c/" ${cuBB_SDK}/cuPHY-CP/cuphycontroller/config/cuphycontroller_P5G_FXN_R750.yaml
sed -i "s/ nic:.*/ nic: 0000:b5:00.0/" ${cuBB_SDK}/cuPHY-CP/cuphycontroller/config/cuphycontroller_P5G_FXN.yaml
sed -i "s/ dst_mac_addr:.*/ dst_mac_addr: 6c:ad:ad:00:04:6c/" ${cuBB_SDK}/cuPHY-CP/cuphycontroller/config/cuphycontroller_P5G_FXN.yaml

sudo -E "$cuBB_Path"/build/cuPHY-CP/cuphycontroller/examples/cuphycontroller_scf P5G_FXN
sudo ./build/cuPHY-CP/gt_common_libs/nvIPC/tests/pcap/pcap_collect
sudo mv nvipc.pcap /var/log/aerial/
sleep infinity
