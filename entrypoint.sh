#!/bin/bash
# 端口检测函数：等待指定 host:port 可达
wait_for() {
    local host=$1
    local port=$2
    while ! nc -z $host $port
    do
        echo "$host:$port 端口连接失败，休眠等待";
        sleep 1;
    done
    echo "$host:$port 检测成功";
}

# 解析参数
declare deps
declare command
while getopts "d:c:" arg
do
    case $arg in
        d)
            deps=$OPTARG;;
        c)
            command=$OPTARG;;
    esac
done

# 对每个 host:port 对进行端口检测
for dep in ${deps//,/ }
do
    host=${dep%:*}
    port=${dep#*:}
    wait_for $host $port
done

echo "端口检测完毕"

# 执行命令
eval $command
