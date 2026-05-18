#!/bin/bash

#传递两个参数
#1.可执行程序的路径名
#2.目录名称 --- 将这个程序的依赖库拷贝到指定目录下
declare depends
get_depends() {
    depends=$(ldd $1 | awk '{if (match($3,"/")){print $3}}')
    #mkdir $2
    cp -Lr ${depends} $2
}

get_depends ./gateway/build/gateway_server ./gateway/depends
get_depends ./media/build/media_server ./media/depends
get_depends ./message/build/message_server ./message/depends
get_depends ./transmite/build/transmite_server ./transmite/depends
get_depends ./identity/build/identity_server ./identity/depends
get_depends ./relationship/build/relationship_server ./relationship/depends
get_depends ./conversation/build/conversation_server ./conversation/depends
get_depends ./push/build/push_server ./push/depends
get_depends ./presence/build/presence_server ./presence/depends

cp /bin/nc ./gateway/
cp /bin/nc ./media/
cp /bin/nc ./message/
cp /bin/nc ./transmite/
cp /bin/nc ./identity/
cp /bin/nc ./relationship/
cp /bin/nc ./conversation/
cp /bin/nc ./push/
cp /bin/nc ./presence/

get_depends /bin/nc ./gateway/depends
get_depends /bin/nc ./media/depends
get_depends /bin/nc ./message/depends
get_depends /bin/nc ./transmite/depends
get_depends /bin/nc ./identity/depends
get_depends /bin/nc ./relationship/depends
get_depends /bin/nc ./conversation/depends
get_depends /bin/nc ./push/depends
get_depends /bin/nc ./presence/depends
