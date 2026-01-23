#!/bin/bash
################################################################################
# PEBS Trace Logging Script
# 功能: 自动捕获 PEBS 采样日志并保存到指定目录
# 作者: Adaptive-PEBS Project
# 日期: 2026-01-22
################################################################################

# 配置
LOG_DIR="/mnt/sas_ssd/lyh/memtis-Nomad/log"
BUFFER_SIZE_KB=10240  # 10MB per CPU
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOGFILE="${LOG_DIR}/pebs_trace_${TIMESTAMP}.txt"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  PEBS Trace Logging Script${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""

# 1. 检查是否为 root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}✗ Please run as root or with sudo${NC}"
    exit 1
fi

# 2. 创建日志目录
echo -e "${YELLOW}[1/8]${NC} Checking log directory..."
if [ ! -d "$LOG_DIR" ]; then
    mkdir -p "$LOG_DIR"
    echo -e "${GREEN}✓${NC} Created directory: $LOG_DIR"
else
    echo -e "${GREEN}✓${NC} Directory exists: $LOG_DIR"
fi

# 3. 挂载 debugfs
echo -e "${YELLOW}[2/8]${NC} Mounting debugfs..."
mount -t debugfs none /sys/kernel/debug 2>/dev/null
if [ -d /sys/kernel/debug/tracing/ ]; then
    echo -e "${GREEN}✓${NC} debugfs mounted at /sys/kernel/debug"
else
    echo -e "${RED}✗${NC} Failed to mount debugfs"
    exit 1
fi

# 4. 检查 trace_printk 是否可用
echo -e "${YELLOW}[3/8]${NC} Checking trace_printk availability..."
if [ -f /sys/kernel/debug/tracing/trace_pipe ]; then
    echo -e "${GREEN}✓${NC} trace_printk is available"
else
    echo -e "${RED}✗${NC} trace_printk not available (CONFIG_FTRACE disabled?)"
    exit 1
fi

# 5. 清空旧数据
echo -e "${YELLOW}[4/8]${NC} Clearing old trace data..."
echo > /sys/kernel/debug/tracing/trace
echo -e "${GREEN}✓${NC} Trace buffer cleared"

# 6. 设置 buffer 大小
echo -e "${YELLOW}[5/8]${NC} Setting buffer size to ${BUFFER_SIZE_KB}KB per CPU..."
echo $BUFFER_SIZE_KB > /sys/kernel/debug/tracing/buffer_size_kb 2>/dev/null
ACTUAL_SIZE=$(cat /sys/kernel/debug/tracing/buffer_size_kb)
echo -e "${GREEN}✓${NC} Buffer size: ${ACTUAL_SIZE}KB per CPU"

# 7. 显示当前配置
echo -e "${YELLOW}[6/8]${NC} Configuration:"
echo -e "  ${BLUE}→${NC} Log file: ${LOGFILE}"
echo -e "  ${BLUE}→${NC} Buffer size: ${ACTUAL_SIZE}KB per CPU"
TOTAL_CPUS=$(grep -c ^processor /proc/cpuinfo)
echo -e "  ${BLUE}→${NC} Total CPUs: ${TOTAL_CPUS}"
echo -e "  ${BLUE}→${NC} Total buffer: $((ACTUAL_SIZE * TOTAL_CPUS / 1024))MB"

# 8. 启动后台记录
echo -e "${YELLOW}[7/8]${NC} Starting background trace logging..."
cat /sys/kernel/debug/tracing/trace_pipe > "$LOGFILE" &
TRACE_PID=$!
echo -e "${GREEN}✓${NC} Trace logging started (PID: ${TRACE_PID})"

# 9. 等待用户操作
echo ""
echo -e "${BLUE}=========================================${NC}"
echo -e "${GREEN}✓ Ready to capture PEBS samples!${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo -e "  ${BLUE}1.${NC} Open another terminal and start Memtis:"
echo -e "     ${GREEN}cd /mnt/sas_ssd/lyh/memtis-Nomad && sudo bash htmm.sh${NC}"
echo ""
echo -e "  ${BLUE}2.${NC} Run your benchmark in a third terminal"
echo ""
echo -e "  ${BLUE}3.${NC} When done, press ${RED}ENTER${NC} here to stop logging"
echo ""
echo -e "${YELLOW}Logging to:${NC} ${LOGFILE}"
echo ""

# 捕获 Ctrl+C
trap "echo -e '\n${YELLOW}Stopping trace logging...${NC}'; kill $TRACE_PID 2>/dev/null; exit 0" INT

# 等待用户输入
read -p "$(echo -e ${RED}Press ENTER to stop logging...${NC})" 

# 10. 停止记录
echo -e "${YELLOW}[8/8]${NC} Stopping trace logging..."
kill $TRACE_PID 2>/dev/null
sleep 1
echo -e "${GREEN}✓${NC} Trace logging stopped"

# 11. 显示统计信息
echo ""
echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  Statistics${NC}"
echo -e "${BLUE}=========================================${NC}"

# 检查文件是否存在且有内容
if [ ! -f "$LOGFILE" ]; then
    echo -e "${RED}✗ Log file not created${NC}"
    exit 1
fi

FILE_SIZE=$(du -h "$LOGFILE" | cut -f1)
TOTAL_LINES=$(wc -l < "$LOGFILE")
PEBS_SAMPLES=$(grep -c "\[PEBS\]" "$LOGFILE" 2>/dev/null || echo "0")

echo -e "${YELLOW}Log file:${NC}"
echo -e "  ${BLUE}→${NC} Path: ${LOGFILE}"
echo -e "  ${BLUE}→${NC} Size: ${FILE_SIZE}"
echo -e "  ${BLUE}→${NC} Total lines: ${TOTAL_LINES}"
echo ""

if [ "$PEBS_SAMPLES" -eq 0 ]; then
    echo -e "${RED}✗ No PEBS samples found!${NC}"
    echo ""
    echo -e "${YELLOW}Possible reasons:${NC}"
    echo -e "  ${BLUE}1.${NC} Memtis not started (ksamplingd not running)"
    echo -e "  ${BLUE}2.${NC} No workload running (no memory access to sample)"
    echo -e "  ${BLUE}3.${NC} trace_printk not added to kernel code"
    echo -e "  ${BLUE}4.${NC} Kernel not recompiled/rebooted with changes"
    echo ""
    echo -e "${YELLOW}Debug:${NC}"
    echo -e "  Check if ksamplingd is running: ${GREEN}ps aux | grep ksamplingd${NC}"
    echo -e "  View raw trace data: ${GREEN}cat $LOGFILE | head -20${NC}"
else
    echo -e "${GREEN}✓ PEBS Sampling Data:${NC}"
    echo -e "  ${BLUE}→${NC} Total samples: ${PEBS_SAMPLES}"
    
    # 事件类型统计
    DRAM_READ=$(grep -c "Event=0" "$LOGFILE" 2>/dev/null || echo "0")
    NVM_READ=$(grep -c "Event=1" "$LOGFILE" 2>/dev/null || echo "0")
    WRITE=$(grep -c "Event=2" "$LOGFILE" 2>/dev/null || echo "0")
    
    echo ""
    echo -e "${YELLOW}Event distribution:${NC}"
    echo -e "  ${BLUE}→${NC} DRAM READ (Event=0):  ${DRAM_READ}"
    echo -e "  ${BLUE}→${NC} NVM READ  (Event=1):  ${NVM_READ}"
    echo -e "  ${BLUE}→${NC} WRITE     (Event=2):  ${WRITE}"
    
    # CPU 分布（前 5 个）
    echo ""
    echo -e "${YELLOW}CPU sampling (top 5):${NC}"
    grep "\[PEBS\]" "$LOGFILE" | \
        sed -n 's/.*\[\([0-9]*\)\].*/\1/p' | \
        sort | uniq -c | sort -rn | head -5 | \
        while read count cpu; do
            printf "  ${BLUE}→${NC} CPU %-3s: %s samples\n" "$cpu" "$count"
        done
fi

echo ""
echo -e "${BLUE}=========================================${NC}"
echo -e "${YELLOW}Commands to analyze log:${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo -e "  ${GREEN}# View first 100 lines${NC}"
echo -e "  cat $LOGFILE | head -100"
echo ""
echo -e "  ${GREEN}# View last 100 lines${NC}"
echo -e "  cat $LOGFILE | tail -100"
echo ""
echo -e "  ${GREEN}# Extract hot addresses (top 20)${NC}"
echo -e "  grep PEBS $LOGFILE | sed -n 's/.*Addr=\\(0x[0-9a-f]*\\).*/\\1/p' | sort | uniq -c | sort -rn | head -20"
echo ""
echo -e "  ${GREEN}# Count samples per PID${NC}"
echo -e "  grep PEBS $LOGFILE | sed -n 's/.*PID=\\([0-9]*\\).*/\\1/p' | sort | uniq -c | sort -rn"
echo ""
echo -e "${BLUE}=========================================${NC}"
echo -e "${GREEN}✓ Logging complete!${NC}"
echo -e "${BLUE}=========================================${NC}"
