#!/bin/bash
#1. 编写端口探测函数，端口连接不上则循环等待
# wait_for 127.0.0.1 3306
wait_for() {
    while ! nc -z $1 $2
    do
        echo "$2 端口连接失败，休眠等待";
        sleep 1;
    done
    echo "$1:$2 检测成功";
}
#2. 对脚本运行参数进行解析，获取到ip ports command
declare ip
declare ports
declare command
while getopts "h:p:c:" arg
do
    case $arg in
        h)
            ip=$OPTARG;;
        p)
            ports=$OPTARG;;
        c)
            command=$OPTARG;;
    esac
done
#3. 通过执行脚本进行端口检测
#${ports //,/ } 针对ports中的内容，以空格替换字符串中的, shell中数组=一种以空格间隔的字符串
for port in ${ports//,/ }
do
    wait_for $ip $port
done

echo "端口检测完毕"

#4. 执行command

eval $command