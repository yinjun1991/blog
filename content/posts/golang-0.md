---
title: 'Go 语言快速上手'
date: 2022-11-22T10:51:05+08:00
draft: false
---

Go 语言是 google 在 2009 年发布的一款静态带有 GC 的高性能语言。它有着以下特点：

1. 语法简洁，概念直白
2. 编译速度快，跨平台
3. 运行速度快，应该与 Java 相当
4. 部署方便，程序打包成一个二进制可执行文件，直接运行
5. 并发使用简单，协程和 channel 比较容易的实现高并发

Go 在云服务领域使用尤为广泛，docker、k8s 等基础组件都是用 Go 实现。
现在国内大公司也越来越多的使用 Go，如滴滴、字节跳动等。

Go 官网提供了教程 [https://tour.golang.org](https://tour.golang.org)，大概 2 小时就能掌握全部语法，建议大家去体验。

Go 官网还提供了在线编辑器 [https://play.golang.org](https://play.golang.org) 方便快速验证代码。

除了 Go 官网，https://go.dev 是官方维护的开发者站点，提供了大量的资料以及包搜索功能。

本文会从这几个方面讲解 Go 的语法：变量、类型、slice、map、函数、控制语句、struct、interface、error、并发。

同时，为了方便理解，有些地方会拿 Java 做参考，但是并未有任何贬低 Java 的意思，各有千秋。

我们先说说 Go 程序怎么跑起来的。

## main

Go 源文件以 `.go` 结尾。

Go 的模块化非常简单，一个目录作为一个 package，该目录内的所有 `.go` 文件开头都要声明 `package xxx` 表示属于该包，该目录下的子目录则不属于该包。

不同于 Java，**Go 的包名和目录路径无关**，可以随意命名，但是约定俗成：包名和目录名保持一致，包的入口文件名和包名保持一致。

后面我们可以看到这类约定俗成的"规范"在 Go 里特别常见。

`package main`  是一个特别的包，main 包的 `main` 方法是程序的入口函数:

```go
// xxx.go
package main

import "fmt"	// 引用标准库的 fmt 包

// main 不接受参数，没有返回值
func main() {
  fmt.Println("hello world")
}
```

为了能编译运行 Go 程序，我们需要安装 Go 的工具包，在 Go 官网 [https://golang.org](https://golang.org) 下载安装即可。

安装之后，打开终端执行命令 `go version`

![image.png](/static/images/golang-0/1_console.png)

看到上图类似的输出，就证明一切准备妥当了。

然后本地新建一个目录，进入目录执行 `go mod init 模块名称` 即可初始化项目。

然后在当前目录创建一个名为 `main.go` 的文件（其他名称也行），文件内容如上。

然后打开终端，在当前目录执行 `go run main.go`  即可看到终端输出 "hello world"。

## 变量

Go 使用 `var`  来声明变量，**类型后置**。

Go 支持类型推导，如果在声明时提供初始值，则可以省去类型。

```go
var name string = "xiaoming"
var name1 = "xiaoming"  // string 类型
var age = 24            // int 类型
var salary = 4000.1     // float64 类型
var ok = false          // bool 类型
```

Go 支持一条语句声明多个**同类型**变量。

```go
var a, b, c string
```

Go 还支持多重赋值。

```go
var name2, age1 = "string", 24
```

多重赋值能极大的提升编码幸福感，比如利用多重赋值交换变量的值。

```go
var a, b = 1, 2
a, b = b, a
```

在函数内部可以使用 `:=`  声明并初始化变量，连 var 都可以省略。

注意：var 和 `:=` 不能一起使用。

```go
func hello() {
  name := "xiaoming"
  age, ok := 24, true
  fmt.Println(name, age, ok)
}
```

没有初始化的变量值是 falsy value，这一点和 Java 类似。

值类型的 falsy value 是 false 0 "" 空结构体，指针和引用类型则为 nil。nil 可以理解为 Java 的 null。

下面章节会讲到值类型、引用类型和指针。

Go 使用 `const`  声明常量，常量的值需要在编译期即可确定，而且只能是 bool、数字/字符、string，不能是其他类型。

```go
const name = "xiaoming"
const welcome = "hello " + name
```

从上面可以看到，Go 语句的结尾没有分号。
Go 官方提供了严格的语法风格和 format 工具，省去了各种语法争吵。

## 类型

Go 支持的基础类型分为 5 大类：布尔、整数、浮点数、复数、字符串，如下所示:

```
bool
uint8   byte
int8
uint16  int16
uint32
int32   rune
uint64	int64
uint    int
float32 float64
complex64   complex128
string
```

Go 支持了 uint，这在处理一些底层的加解密和编码计算是非常友好的。

uint8 和 byte 等价，取值范围 `[0, 255]`。

Go 支持 UTF8 编码，用 int32 表示一个字符，而且使用和 int32 等价的 `rune`  作为类型名称，应该是想和 C 语言的 ASCII 范围的 char 作区分。

Go 支持复数类型 complex64 complex128，复杂的科学计算也是可以胜任的。

前面我们提到变量初始化时可以省略类型，但是当变量类型和推导类型不匹配时就不能省略了。

```go
var abc uint32 = 12
```

如上所示，如果省略类型 uint32，abc 会被推导为 int 类型。

必须要说的是 Go **禁止隐式类型转换**。

```go
var a = 10          // int
var b int32 = 20    // int32

b = a	            // error
a + b	            // error

var c = 10.2	// float64
b + c           // error
```

不同数值之间计算需要使用类型转换，如 `b = int32(a)`

禁止隐式类型转换提高了安全性，也增加了一些类型转换的繁琐。总体来看，利大于弊。

### string

string 在 Go 中是值类型，字符串本身不可修改。

```go
var str = "hello"
str == "hello"	// 使用 == 比较
str[0] = "H"      // error
```

Go 的基础类型都是值类型，而且没有 Java 的装箱拆箱机制。这意味着 Go 不能像这样获操作字符串：

```go
str.StartsWith("he")
str.length
```

只能使用函数：

```go
strings.HasPrefix(str, "he")    // true
len(str)    // 5
```

Go 标准库提供了 strings 包来操作字符串。

`len` 则是 Go 内置的函数，后面会讲到。

### 指针

Go 支持指针，基本用法同 C 语言。

```go
var a = 10
var p *int = &a
*p == 10
*p = 11
```

Go 不仅将类型后置，而且类型的描述按照从左到右的顺序来理解，和 C 语言大不相同。

比如 `*int` 从左到右就是：指针、int，首先是个指针，其次指向 int。

再比如指针数组和数组指针，在 Go 中如下所示：

```go
var p1 []*int
var p2 *[]int
```

依然按照从左到右的顺序，p1 首先是个数组，其次元素是指针，指针指向的是 int 类型。p2 首先是个指针，指向数组，数组元素是 int 类型。

刚开始感觉有些别扭，但是理解之后发现真的很通顺，再也不会傻傻分不清楚指针数组还是数组指针了。

Go 官方 blog 有一篇文章专门讲到了类型语法：https://blog.golang.org/declaration-syntax

Go 虽然支持指针，但是对指针的使用做了很多限制，比如不能对指针本身进行计算，以下写法错误：

```go
p++
```

这些限制导致 Go 的指针就像 Java 的引用，完全没有 C 那么魔法和痛苦。

但是 Go 的指针可以用于任何类型，包括基础类型。

### 值类型、引用类型、指针类型

Go 的类型我们就讲完了，在 Go 中所有类型可以分成三类：值类型、引用类型、指针类型。

值类型简单理解就是可以用 `==` 判断相等的类型，包括基础类型、string、数组、struct。值类型的默认值是该类型的空值或者叫 falsy value。

比如 [4]int 默认值就是 4 个元素都是 0 的数组，而不是 nil。

引用类型真的就类似 Java 的引用类型了，常见的包括 slice、map、interface、channel，这些类型后面会讲到。

指针类型和引用类型有些相似，它们的默认值都是 nil。

### 运算符

Go 也有 `++`  和 `--` ，但是它们不是运算符，而是语句 `+= 1`  和 `-= 1`  的简写。

```go
a = b++             // error
fmt.Println(a++)    // error
```

`b++` 其实是 `b += 1`，是一个语句而不是表达式，所以不能放在表达式的位置

另外，Go 不支持 `? :`  三目运算符，只能用 if 语句。

其他的运算符就不赘述了。

## 数组与 slice

为了方便下文涉及到类型的描述，我们使用 T 代表类型。注意：Go 目前（1.15）还不支持泛型，这里只是用来描述。

数组在 Go 中是**值类型**，用类似 `[4]T`  的方式表示，方括号内指定数组长度，后面紧跟元素类型。

数组长度也是类型的一部分，所以 `[3]T` 和 `[4]T` 是不同类型。

**数组创建之后长度不可改变**。

声明并初始化：

```go
var arr1 [4]int = [4]int{10, 20, 30, 40}
```

Go 使用 `T{}` 来创建一个该类型的值。

可以用 `...` 代替长度，由编译器推导：

```go
var arr2 = [...]int{10, 20, 30, 40}
```

还可以在初始化时指定索引初始值：

```go
var arr3 = [...]int{
  100: 9527,
}
```

以上代码会声明一个长度为 101 的数组，最后一个元素值为 9527。

用 `==` 判断数组是否相等。

```go
arr1 == arr2
```

对数组的下标访问不能越界。

### slice

slice 可以理解为不定长数组，类似 Java 的 ArrayList。

slice 用 `[]T` 表示，方括号内不包含长度。

slice 是引用类型，初始值为 nil。

```go
var s []int // s == nil
```

从数组、字符串和 slice 中使用 `[start:end]`  可以构建 slice。

```go
var a = [4]int{10, 20, 30, 40}
var b = a[1:3]

fmt.Println(b)  // [20 30]
```

可以看到 slice **不包括** end 位置的元素。

start 默认为 0，end 默认**包含**最后一个元素，创建 slice 时可以省略 start 和 end。

```go
var buffer = "hello world"
var slice1 = buffer[1:3]
var slice2 = buffer[2:8]
var slice4 = buffer[:]
```

可以基于 slice 创建 slice。

```go
var slice3 = slice2[3:6]
```

可以将 slice 看作是基于底层存储数组（下文简称为 buffer）的视图，如下图所示：

![image.png](/static/images/golang-0/2_slice.png)

理解这张图非常重要，slice 本身结构大致如下：

```go
{
  _data,	// 指向底层存储数组
  start,	// slice 的开始位置
  end,	// slice 的结束位置
}
```

可见 slice 本身是非常轻量的，当多个 slice 共用一个 buffer 时，会明显的减少内存开支。

可以直接创建 slice，Go 会自动创建 buffer。

```go
var s = []int{10, 20, 30}
```

还可以使用内置的 `make` 函数创建 slice。

```go
var s2 = make([]int, 4, 10)
```

上面的代码创建了一个长度为 10 的 buffer，s2 的长度则为 4。关于 make 的使用，内置函数章节会讲到。

对 slice 的修改会作用到 buffer，因此也会影响基于该 buffer 的其他 slice。

```go
var s = []int{10, 20, 30}
var s1 = s[1:2]
var s2 = s[1:3]
s1[1] = 200		// s2[0] == 200
```

Go 提供了一些内置函数来操作 slice：

- len 获取 slice 长度。当 slice 值为 nil 时，len 返回 0
- append 向 slice 追加元素，返回新 slice
- cap 获取 slice 底层 buffer 的容量

```go
ss := make([]int, 4, 10)
len(ss)	// 4
cap(ss)	// 10

ss = append(ss, 2)
len(ss)	// 5
cap(ss)	// 10
```

如果 slice 底层 buffer 容量不够的话，Go 会新建一个容量足够的 buffer，并且将原 buffer 的数据复制到新 buffer 中。

```go
buffer := [...]int{10, 20, 30}
a1 := buffer[:]
a2 := buffer[0:2]
a1 = append(a1, 40, 50, 60)
a1[0] = 100
a2[0]	// 10
```

![image.png](/static/images/golang-0/3_slice.png)

看懂上图就明白了:

1. a1 和 a2 初始都指向 buffer。
1. 向 a1 append 数据时，由于 buffer 容量不足，所以新分配了 buffer2，并且将 buffer 的数据复制到 buffer2 中，再将 40 50 60 append 到 buffer2。
1. 修改 a1[0] 会修改 buffer2，但是由于 a2 仍然指向 buffer，所以不受影响。

## map

Go 使用 `map[keyT]valueT`  表示 map。

map 属于引用类型，初始值为 nil。

map **非线程安全**，对 map 并发读写不加锁会造成 panic。

```go
var m map[string]int // m == nil
```

可以这么初始化 map：

```go
var m2 = map[string]int{}

var ages = map[string]int{
  "xiaoming": 24,
  "xiaozhang": 22,
}
```

内置函数 `make` 除了能创建 slice，还能创建 map：

```go
var ages = make(map[string]int, 10)	// 预先分配10 个 key 的空间
```

map 的使用非常简洁：

```go
ages["xiaoming"]            // 20
ages["xiaoli"] = 25         // 添加 key
delete(ages, "xiaoming")    // 删除 key

age, ok := ages["xiaoming"] // ok 为 false
age, ok = ages["xiaoli"]    // ok 为 true
```

使用多值返回，第二个值通常取名为 ok，表示元素是否在 map 中。

## 函数

Go 使用 `func` 声明函数，而且支持闭包、高阶函数、柯里化等特性。

```go
func add(a, b int) int {
  return a + b
}
```

函数参数同样是类型后置，连续的参数类型相同时，可以省略前面的类型。

函数返回类型在参数列表和函数体之间，若无返回值，可以省略返回类型。

```go
func Print(message string) {

}
```

### 变长参数

函数支持变长参数，变长参数只能是最后一个参数：

```go
func sum(nums ...int) int {
  acc := 0
  for _, n := range nums {
    acc += n
  }
  return acc
}
```

变长参数这么调用：

```go
nums := []int{1, 2, 3}
sum(nums...)
```

可以用 `...` 解构 slice。

### 多返回值

函数支持多返回值：

```go
func swap(a, b int) (int, int) {
  return b, a
}
```

### 返回值命名

函数支持返回值命名，和参数类似：类型后置、连续的返回值类型相同时，可以省略前面的类型。

返回值命名，相当于在函数体的最前面声明了变量，但是并未初始化。

```go
func sum(nums ...int) (acc int) {
  for _, n := range nums {
    acc += n
  }
  // 由于返回值 acc 已经声明，直接 return 是可以的
  return
}
```

当然，声明的返回值我们可以不用：

```go
func swap(a, b int) (x, y int) {
  return b, a
}
```

上面的事例，我们返回了 b, a 而没有用 x, y，这是可以的。

### 匿名函数

Go 支持匿名函数。

```go
func() {
  fmt.Println("匿名函数立即执行")
}()
```

Go 将函数看作为值，函数签名则可以看作是函数的类型。如下：

```go
var abc func (n int)

abc = func (n int) {
  fmt.Println(n)
}

var bcd func (n int)

bcd = abc
```

虽然函数也是值，但是函数之间却不能比较是否相等，函数只能和 nil 进行比较。

```go
fmt.Println(abc == bcd) // error

fmt.Println(abc == nil) // false
```

### 高阶函数

由于 Go 将函数看作是值，所以 Go 有一些函数式语言的特性。

Go 支持高阶函数：

```go
func forEach(nums []int, do func(n int)) {
  for _, n := range nums {
    do(n)
  }
}

forEach([]int{1, 2, 3}, func (n int) {
  fmt.Println(n)
})
```

Go 还支持柯里化及闭包：

```go
func curry(init_value int) func (...int) int {
  return func(nums ...int) int {
    ret := init_value
    for _, num := range nums {
      ret += num
    }
    return ret
  }
}

var c = curry(1)
c(2, 3) // 6
```

但是 Go 不支持函数嵌套声明。

```go
func outer() {
  func inner() {  // error
  }
}

// 匿名函数赋值是可以的
func outer2() {
  inner := func() { // ok

  }
  return inner
}
```

Go 函数不支持以下特性：

1. 可选参数
2. 参数默认值
3. 函数重载

在所有函数之外的代码，我们可以简称为全局代码。

全局代码只能有变量/常量初始化、类型/函数/方法声明语句，不能有其他的计算语句、函数执行语句。

### defer

在 Go 函数内可以使用 `defer` 调用另一个函数，该函数会在当前函数返回之前执行。通常可以用来释放打开的文件等资源。

```go
func write(file string, data []byte) (int, error) {
  f, err := os.Open(file)
  if err != nil {
    return 0, err
  }

  // f.Close() 会在 return 之后，write 函数真正返回之前执行
  defer f.Close()

  return f.Write(data)
}
```

如果存在多次 `defer` 调用，那么最终执行顺序与 defer 调用顺序相反，可以看作是这些调用都被压入栈中，再依次弹出执行。

```go
func testDefer() {
  for i := 0; i < 3; i++ {
    defer fmt.Println(i)
  }
}
```

以上代码最终会打印:

```text
2
1
0
```

### 内置函数

Go 有很多内置的函数，常见的有 len、cap、append、delete、make、copy、init、close、new 等。

涉及到 channel 的知识，我们会在并发章节讲到。

#### len

len 不仅可以用于 slice，还可以用于字符串、数组、map 和 channel。

```go
len("hello")    // 字符数 5
len([]int{1, 2, 3}) // 3
len([...]int{1, 2, 3})  // 3
len(map[int]int{1: 1, 2: 2})    // key 数量 2
```

#### cap

cap 用来获取数组、slice、channel 总容量

#### append

向 slice 中追加元素

#### delete

从 map 中删除 key

#### make

用来创建 slice、map 和 channel

```go
s := make([]int, 3, 10)
m := make(map[string]string, 4)
ch := make(chan int, 10)
```

#### copy

在 slice 之间复制数据

```go
nums := [...]int{1, 2, 3}
nums2 := make([]int, 4)
copy(num2, nums)	// nums 复制到 nums2
```

#### init

每个包可以声明多个 init 函数，这些 init 函数会在 package 加载时按照顺序执行

```go
package database

import "fmt"

func init() {
  fmt.Println("hello")
}

func init() {
  fmt.Println("world")
}
```

#### close

关闭 channel

#### new

`new(T)`  分配内存并且返回指针，极少使用。

```go
b := new(int)	// b 是 *int 类型
*b = 10
```

## 控制语句

Go 的控制语句分为 if for switch goto select 5 种，外加 break 和 continue，没有 while 和 do while。

select 用于 channel，在并发章节再说它。

### if

if 的使用和 C 语言差不多：

```go
if a < b {
  // a is not max
} else if a < c {
  // c is max
} else {
  // a is max
}
```

条件不用括号包围，但是花括号是必需的。for switch 语句也是同样的风格。

if 还可以声明并初始化变量，该变量的作用域仅限于 if 语句及相关联的 else 语句，非常贴心。

```go
if data := fetch(); data != nil {
  // data can be accessed in there
}
// data cannot be accessed in there
```

如果我们在条件判断中用了赋值，Go 会报错的：

```go
if a = b {  // error
}
```

### for

Go 的循环只有 for，但是功能丝毫不弱，常用方式如下：

```go
for a < b { // 类似 while(a < b)
}

for {   // 类似 while(true)
}

// 典型 for 遍历
// 3个语句可以省略，但是分号不能省略
for i := 0; i < len(arr); i++ {
}
```

Go 还支持 `for range` 遍历，可以用于字符串、slice、数组、map 和 channel，类似 len 函数的使用范围。

```go
// 遍历索引和元素
for i, num := range nums {
}

// 只遍历索引
for i := range nums {
}

// 只遍历元素，忽略索引
for _, num := range nums {
}

// 遍历 map 的 key value
for k, v := range some_map {
}

// 只遍历 key
for k := range some_map {
}

// 只遍历 value
for _, v := range some_map {
}
```

Go 不允许存在未使用的变量，对于不想使用的值，我们可以赋值给 `_`，表示忽略该值。

break continue 的用法同 C 语言。

Go 支持对 for 循环加标签，标签可以应用于 break 和 continue。

```go
START:
for {
  for {
    break START	// 直接跳出外层循环
  }
  continue START
}
```

### switch

每个 case 之后不需要加 break。

如果需要联合多个 case，可以用 `fallthrough` 关键字声明。

```go
score := 'B'
switch score {
  case 'A':
    fmt.Println("Great")
  case 'B':
    fallthrough
  case 'C':
    fmt.Println("Well")
  default:
    fmt.Println("Bad")
}
```

上面的事例，'B' 和 'C' 都会打印 "Well"。

switch 的条件可以省略，此时 case 支持条件计算，类似 if。

```go
score := 'B'
switch {
  case score == 'A':
    fmt.Println("Great")
  case score == 'B' || score == 'C':
    fmt.Println("干巴爹")
  default:
    fmt.Println("Bad")
}
```

### goto

Go 保留了 goto 语句，但是只能跳转到之前已声明的标签。

```go
start := time.Now()

WAIT:
fmt.Println("wait")
if time.Now().Sub(start) < 100 {
  goto WAIT	// WAIT 只能在之前定义
}
```

## struct

Go 使用 `struct`  声明符合结构，struct 属于值类型。

```go
type Person struct {
  name string
  age int
}
```

由于需要使用 type 和 struct 两个关键字创建 struct，这一点也是被社区诟病。

函数体内也可以声明 struct，不过该 struct 只能在函数内可见。

```go
func main() {
  type Test struct {
  }

  test := &Test{}
}

// Test 在 main 之外不可见
```

struct 没有构造方法，可以用 `T{}`  的方式创建。

```go
p1 := Person{}	// 各字段都是默认值
```

可以声明局部字段的值：

```go
p2 := Person{
  name: "xiaoming",	// 只给局部字段赋值，不能省略字段名
}
```

如果所有字段都按照顺序提供了值，可以省略字段名：

```go
p3 := Person{"xiaoming", 24}
```

通过 `.` 访问 struct 字段。

```go
p2.name // "xiaoming"
```

struct 是值类型，可以用 `==` 比较。

```go
p4 := Person{"xiaoming", 24}
p3 == p4
```

更常用的是创建 struct 指针：

```go
pp := &p1

pp1 := &Person{}

pp2 := &Person{
  name: "xiaoming",
}

pp3 := &Person{"xiaoming", 24}
```

指针访问字段也是用 `.` 而不是 C 的 `->`。

```go
pp2.name
```

上面的代码会被 Go 编译器改成 `(*pp2).name`。

### 工厂函数

Go 约定俗成使用 New 加上类型名称，作为工厂函数的名称。

```go
func NewPerson(name string) *Person {
  return &Person{
    name: name,
  }
}

pp4 := NewPerson("xiaoming")
pp4.age = 24
```

### method

可以给 struct 添加方法：

```go
func (p Person) hello() {
  fmt.Println("hello", p.name)
}

p2.hello()
```

如上所示：方法的格式类似函数，只是在 func 和方法名称之间添加了一个变量，官方称之为 `receiver` ，约定俗成 receiver 变量名是类型名的首字母小写。

方法内使用 receiver 来表示当前 struct 值，Go 没有 this self super 这些关键字。

**其实 method 只是一个普通方法，额外接收了一个 receiver 变量而已。**

也就是说上面的 hello 方法可以看作是：

```go
func hello(p Person) {

}
```

下面这个例子，能证明上面这句话：

```go
/* 类似
 * func set_name(p Person, name string) {
 *   p.name = name
 * }
 */
func (p Person) set_name(name string) {
  p.name = name
}

p2.set_name("wangxiaoming")

fmt.Println(p2.name)	// xiaoming
```

p2 的 name 字段并没有更新，这是为什么呢？原因如下：

1. struct 是值类型，p 作为 p2 的复制体传递给了 set_name
2. set_name 改变了 p 的 name，但是 p 只是 p2 的复制体，p2 并不受影响

怎么解决这个问题呢？

```go
func (p *Person) set_name(name string) {
  p.name = name
}
```

将**指针类型**作为 receiver，p2 传递给 p 的是指针，修改 p 指向的 struct，也就是 p2。

`p2.set_name`  实际会被编译器转换为 `(&p2).set_name`，这些优化却带来一些理解上的困惑。

为了避免这些困惑，同时减少方法调用时的开销，建议 receiver 一律使用指针类型。

### 组合

Go 不支持 struct 的继承，但是支持 struct 组合。

组合这么表示：

```go
type Employee struct {
  Person
  salary float64
}
```

Employee `has-a` Person。

这么创建 Employee 类型值：

```go
e := Employee{
  Person: Person{
    name: "xiaoming",
    age: 24,
  },
  salary: 1000.89,
}
```

Employee 可以直接访问 Person 的字段和方法：

```go
e.name
e.age
e.hello()
e.set_name("wangxiaoming")
```

看起来就像是 Employee 继承了 Person 一样，但这实际上是 Go 编译器做了简写的优化。

上面的代码实际上是：

```go
e.Person.name
e.Person.age
e.Person.hello()
e.Person.set_name("wangxiaoming")
```

所以我们不能这样写：

```go
// error 类型不匹配
var p Person = e
```

组合是 `has-a` 而不是 `is-a` 的关系。

可以覆盖内嵌 struct 的方法：

```go
func (e Employee) hello() {
  e.Person.hello()
  fmt.Println("i am an employee")
}
```

### 组合的语法歧义

Employee 组合 Person 的方式，相当于声明了一个字段名和类型名都是 Person 的字段。

e.name 会被编译器转换为 e.Person.name。

但是当我们真的按照以下方式声明 Employee 时，却不能直接引用内嵌 struct 的字段和方法。

```go
type Employee struct {
  Person Person
  salary float64
}

e.name	// error
```

这是 Go 另一个让人感到歧义的地方。

### 组合的冲突

```go
type ABC struct {}

func (abc ABC) hello() {
}

type ABCPerson struct {
  ABC
  Person
}

abc := ABCPerson{}
abc.hello()		// error
```

上面的代码会报错，因为编译器不知道 abc.hello 该调用 abc.Person.hello 还是 ee.ABC.hello。

如果 C 内嵌了 A 和 B，A 和 B 拥有同名的字段或者方法，访问这些字段或者方法时，需要说明用 A 还是 B 的，否则会报错。

### 组合 VS 继承

Go 的组合相比继承，在语法层面更加清晰简洁，没有 this super 等关键字带来的烧脑问题。

但是继承的缺失不利于某些模式的实现，如下 Java 的代码：

```java
abstract class Cookie {
  private String name;

  Cookie(String name) {
    this.name = name;
  }

  abstract void doCookie();

  void cookie() {
    System.out.println("I'm cooking " + this.name);
    this.doCookie();
  }
}

class Pancake extends Cookie {
  Pancake() {
    super("pancake");
  }

  @Override
  void doCookie() {
    System.out.println("cook pancake");
  }
}

class Cake extends Cookie {
  Cake() {
    super("cake");
  }

  @Override
  void doCookie() {
    System.out.println("cook cake");
  }
}

public class Main {
  public static void main(String[] args) {
    Pancake p = new Pancake();
    p.cookie();

    Cake c = new Cake();
    c.cookie();
  }
}
```

以上是典型的通过继承实现的模板方法模式，子类可以访问父类的属性/方法，父类也可以访问子类最终实现的方法。

而在 Go 中实现同样的逻辑则会繁琐很多，因为内嵌 struct 没法访问外部 struct 的字段/方法。

除非显示的将外部 struct 的指针再赋给内嵌 struct 的某个字段，比如：

```go
type A struct {
  wrapper *interface{}
}

type B struct {
  A
}

b := &B{}
b.wrapper = b
```

上面的代码显然更丑更绕，而且 A 的 wrapper 字段不能确定类型，因为 A 不知道会被谁组合。

## interface

interface 是一堆方法签名的组合，和 java 类似。

interface 的名称约定俗成用 `er`  结尾，比如标准库的 `Stringer`。

这么声明 interface：

```go
type Runner interface {
  Run()
}
```

Go 提倡 interface 保持精简，在 Go 的源码中有大量的 interface 只包含一两个方法。

注意下面的代码，这是 Go interface 甚至整个 Go 语言的精髓。

```go
func (p Person) Run() {
  fmt.Println(p.name, "is running")
}

var r Runner = Person{}
r.Run()
```

Person 类型实现了 Runner 的所有方法，所以 Person 类型的值可以赋值给 Runner 变量。

在 Go 中，struct 不需要使用 `implements`  表示实现了某个 interface，interface 也不知道哪些 struct 实现了自己。

这样就实现了 struct 和 interface 解耦，堪称精妙。

我们看个例子感受一下：

假设我们写了一个 DB 类型给其他同事使用：

```go
package database

type DB struct {}

func (d *DB) Write() {}
func (d *DB) Read() {}
func (d *DB) Clean() {}
func (d *DB) Sync() {}
```

同事 A 只需要 Write 方法，同事 B 只需要 Sync 方法，他们在各自的代码里定义了 interface。

这样可以保持接口精简，省略不需要的方法，更安全也更方便 mock 和测试。

```go
// 同事 A
// a/interfaces.go
package a

type DBA interface {
  Write()
}

func Init(db DBA) {
}

// 同事 B
// b/interfaces.go
package b

type DBB interface {
  Sync()
}

func Init(db DBB) {
}
```

同事们定义了自己的接口，我的代码却不需要修改。

```go
package main

func main() {
  db := database.DB{}
  a.Init(db)	// db 实现了 DBA 接口
  b.Init(db)	// db 实现了 DBB 接口
}
```

如果是在 java 中，同事 C 增加接口 DBC，或者接口名称更新时我的代码也要跟着改。

因为我硬编码了 `class DB implements DBA, DBB`。

如果我不改代码，同事们就只能这么做：

```java
// a/DBAImpl.java
// 不能用 DB，因为 DB 没有实现 DBA，只能加一个中间层
public class DBAImpl implements DBA {
  private DB db;

  public DBAImpl(DB db) {
    this.db = db;
  }

  public Write() {
    this.db.Write();
  }
}

public void InitA(DBA dba) {
}

// b/DBBImpl.java
public class DBBImpl implements DBB {
  private DB db;

  public DBBImpl(DB db) {
    this.db = db;
  }

  public Sync() {
    this.db.Sync();
  }
}

public void InitB(DBB dba) {
}

public static void main(String[] args) {
  DB db = new DB();

  DBAImpl dba = new DBAImpl(db);
  InitA(dba);

  DBBImpl dbb = new DBBImpl(db);
  InitB(dbb);
}
```

是不是很绕？多了很多代码，DBAImpl 和 DBBImpl 属于没有意义的代码。

假设 DB 属于第三方包，我们仍然可以按需定义自己的 interface，而不用修改 DB 的代码。

这就是 Go interface 解耦的威力。

### 缺点

Go interface 并非没有缺陷，比如：

```go
type Human interface {
  Speak()
}

type Animal interface {
  Speak()
}

// Person 想实现的是 Human 接口
func (p Person) Speak() {
}

// 出乎意料 Person 竟然也实现了 Animal 接口
var b Animal = Person{}
```

Person 实现了 Animal 接口，这并不符合预期，可能会带来意外的 bug，如果是显式 implements 就不会出现这个问题。

非显式实现接口，还可能导致不能及时的发现类型不匹配的错误。

假设 Runner 接口更新如下：

```go
type Runner interface {
  Run() error
}
```

Person 此时不再实现 Runner 接口了，由于不是显式声明接口实现，我们看 Person 类的代码时是不会发现这个错误的，只有在进行类型匹配的地方编译器才会报错：

```go
var r Runner = Person{}	// error
```

综上所属，Go interface 有它的缺点，但是优点远大于缺点。

### interface{}

Go 使用 `interface{}` 空接口表示 any 类型，如下：

```go
var n interface{} = 1
n = "hello"
```

### 类型判断

Go 使用 `v.(T)` 判断 interface 的值是不是某个类型，比如：

```go
var r Runner = Person{}

p, ok := r.(Person)

// ok 为 true 表示类型判断正确
// p 被转换为 Person 类型
if ok {
  fmt.Println(p.name)
}
```

可以用 `.(type)` 配合 switch 语句判断多种类型：

```go
switch r.(type) {
  case Person:
    fmt.Println("is a Person")
  case SomeOtherType:
    fmt.Println("is SomeOtherType")
  default:
    fmt.Println("unknown type")
}
```

### 组合

interface 同样支持组合，如下：

```go
type Reader interface {
  Read(buf []int) (int, error)
}
type Writer interface {
  Write(data []int) (int, error)
}

type ReadWriter interface {
  Reader
  Writer
}
```

struct 可以组合 interface，如下：

```go
type People struct {
  Runner	// 组合了 interface
  job string
}

var p People = People{
  Runner: Person{"xioming", 24},
  job: "programmer",
}
```

### type

Go 支持使用 `type` 声明新类型，比如声明 struct 和 interface。

可以基于现有类型声明一个新类型，给新类型添加方法。

这么做的意义，通常是给需要的类型起一个更有意义的名称，更恰当的应用场景。

如下：

```go
type Id uint64

func (i Id) IsAdmin() bool {
  return i == 1
}

var userId Id = 9527

userId.IsAdmin()
```

我们创建了新类型 Id。

新类型和它的真实类型并不相同：

```go
var num uint64 = 123
var userId Id = num // error
```

#### 枚举

Go 并不支持枚举，通常我们使用如下方式来实现：

```go
type Gender byte

const (
  Gender_Man = iota // 0
  Gender_Woman  // 1
)
```

用一对圆括号包围相关的常量/变量。

关键字 `iota` 我们可以理解为 0，但是它在之后的每一行会递增。

上面的代码中我们省略了 Gender_Woman 的赋值，相当于 `Gender_Woman = iota`，由于 iota 会递增，所以 Gender_Woman 的值是 1.

由于 iota 的特性，我们还可以这么做：

```go
type Flag byte

const (
  Flag_W = 1 << iota // 1
  Flag_R             // 2
)
```

#### 类型别名

Go 还支持 `type A = B`  定义类型别名。

类型别名并不会创建新类型，所以不能给类型别名添加方法。

```go
// Go 内置
type byte = uint8
type rune = int32
```

### error

Go 的错误处理一直为人诟病，Go 没有 try catch finally，而是约定俗成，声明函数的最后一个返回值为 error 类型，调用方判断是否发生错误。

```go
func fetch(url string) (res Response, err error) {

}

data, err := fetch("/api/xxx")
if err != nil {
  fmt.Println(err)
  return
}
```

error 是 Go 内置的 interface，并不是 struct。

```go
type error interface {
  Error() string
}
```

任何实现 error 的类型都可以作为 error。

比如：

```go
type ErrorCode int

var errMessage = map[ErrorCode]string{
  404: "Not Found",
}

func (e ErrorCode) Error() string {
  msg, ok := errMessage[e]
  if ok {
    return msg
  }

  return "Unknown"
}

var e ErrorCode = 404
fmt.Println(e)

e = 500
fmt.Println(e)
```

### panic 与 recover

通常程序出错时，比如空指针、数组越界，我们成为 crash 或者崩溃，Go 中则称为 `panic`。

Go 提供了 `panic` 函数用来停止执行后续代码，类似 Java 的 `System.exit()`。

Go 还提供了 `recover` 函数用来捕获造成 panic 的 error，类似 Java 的 catch，如果没有 panic 则返回 nil。

recover 的调用方只能被 defer 调用，如下的匿名函数：

```go
func main() {
  do()
  // 会执行，因为 do 内部的 panic 已经被处理了
  fmt.Println("main done")
}

func do() {
  defer func() {
    if err := recover(); err != nil {
      // main 发生了 panic
      fmt.Printf("panic: %v\n", err)
    }
  }()
  panic("do panic")
  // 以下代码不会被执行
  fmt.Println("do done")
}
```

## 并发

Go 没有线程，但是有协程，简称 goroutine。Go runtime 实现了 goroutine 和 native thread `m:n` 的调度。

使用关键字 `go`  执行函数即可创建 goroutine，如下：

```go
func do_some_thing() {

}

// 开启一个并发的 goroutine
go do_some_thing()
```

主线程退出时，所有的 goroutine 也都会退出。

goroutine 的创建很方便，但是也缺乏一些高级功能，比如 goroutine 命名或者唯一 id、threadLocal 存储、暂停、让出执行权等。

goroutine 并发执行时，可能会出现意想不到的结果，如下：

```go
func main() {
  for i := 0; i < 10; i++ {
    go func () {
      time.Sleep(10)
      // 猜猜打印什么？
      fmt.Println(i)
    }()
  }

  for i := 0; i < 10; i++ {
    go func(n int) {
      time.Sleep(10)
      // 猜猜打印什么？
      fmt.Println(n)
    }(i)
  }

  // 阻塞主线程
  time.Sleep(1000)
}
```

### channel

channel 类似于管道，goroutine 可以从 channel 中读取/写入数据，这样就实现了 goroutine 之间的通信。

channel 有容量限制，一旦创建容量不可修改。如下：

```go
// channel 容量是 10
ch := make(chan int, 10)

// channel 容量是 0
ch2 := make(chan int)

const total = 10

go func() {
  for i := 0; i < total; i++ {
    // 向 channel 中写入
    ch <- i + 1
  }

  // 用完 channel 要关闭
  close(ch)
}()

go func() {
  for i := 0; i < total; i++ {
    // 从 channel 中读取
    fmt.Println(<- ch)
  }
}()

// 阻塞主线程
time.Sleep(1000)
```

当 channel 的容量满了，写入方会被阻塞住。
当 channel 的元素个数为 0 时，读取方会被阻塞住。

channel 一旦被关闭，读取方会获取源源不断的 falsy value。
channel 一旦被关闭，不可以再次关闭，不可以再写入，否则会 panic。

### 只读只写 channel

channel 还可以是只读和只写类型。

```go
// 只写 channel
var wch chan <- int

// 只读 channel
var rch <- chan int

// 读写类型的 channel 可以赋值给只读、只写类型
c := make(chan int)
wch = c
rch = c
```

### 遍历 channel

使用 for range 遍历 channel，**当 channel 被 close 时遍历结束。**

所以上面遍历 channel 的代码可以改成：

```go
go func() {
  for n := range ch {
    fmt.Println(n)
  }
}()
```

### select

当有多个 channel 可以读取写时，可以使用 select 随机选择一个操作。

```go
c1 := make(chan int, 10)
c2 := make(chan int, 2)

var n int
select {
  case c1 <- n * 2:
    fmt.Println("write", n*2)
  case n = <- c2:
    fmt.Println("read", n)
  default:
    // 当 case 都被阻塞时，会走 default
    fmt.Println("idle")
    // 退出 select 语句
    break
}
```

Go 程序中经常用 channel 发送控制信号，如下：

```go
// quit 管道用来发送控制信号，类型是空 struct
quit := make(chan struct{})

nums := make(chan int)

go func() {
  var ret int
  Loop:
  for {
    select {
      // quit 读取到值就退出
      case <-quit:
        // 注意这里的标签
        break Loop
      case n := <-nums:
        ret += n
    }
  }
  fmt.Println("sum is ", ret)
}()

for i := 0; i < 10; i++ {
  nums <- i
  if rand.Intn(10) > 5 {
    quit <- struct{}{}
  }
}
```

上面的 `break Loop` 退出 for 循环，不加 Loop 标签的话 break 只能退出 select 语句。

## package 可见性

在文章开始，我们说到 Go 将目录作为一个 package。

Go 没有 private public 之类的关键字，同一个 package 中的内容相互可见。可以把一个 packae 内的所有内容看作是写在一个大文件中。

package 中的内容哪些对外可见，是由**名称的首字母大写**决定，首字母大写则包外可见，小写包外不可见。

名称指的是：全局变/常量名、struct/interface 类型名称、struct 字段名、函数/方法名。

如下：

```go
package database

// 包外可见
const Version = 2

// 包外不可见
const prefix = "user_"

// db 类型包外不可见，所以不能通过 db{} 方式实例化
type db struct {
  // err 字段包外不可见
  err error
}

// Write 方法包外可见
func (d *db) Write(data []byte) (n int, err error) {

}

// clean 方法包外不可见
func (d *db) clean() {

}

// New 函数包外可见
func New() *db {
  return &db{}
}
```

在其他 package 中，我们可以访问的内容如下：

```go
package main

import "database"

func main() {
  // ok
  database.Version

  // error 不可访问 prefix 变量
  database.prefix

  // error 不可访问 db 类型
  var db2 *db

  // ok
  db := &database.New()

  // error 不可访问 err 字段
  db.err

  // ok
  db.Write([]byte{1, 2, 3})

  // error 不可访问 clean 方法
  db.clean()
}
```

利用上面的可见性规则，我们可以创建包外可见，但是不可被实现的 interface，如下：

```go
package database

type Cleaner interface {
  Clean()
  // 由于该方法在包外不可见
  // 所以包外的 struct 不可能实现 Cleaner 接口
  noop()
}

type dbCleaner struct {}

func (d *dbCleaner) Clean() {}

// 缺点，需要带一个非常丑陋的无意义的方法
func (d *dbCleaner) noop() {}
```

上面所示的 Cleaner 接口不可能在包外被实现。但是缺点显而易见，database 包内实现 Cleaner 接口的 dbCleaner 需要带有丑陋的无意义的 noop 方法。

以上即为 Go 的绝大部分语法。

## 个人看法

我个人是非常喜欢 Go 语言的。它带有浓浓的 C 语言和 Unix 哲学味道，
修正了 C 的很多语法缺陷以及容易歧义和困惑的语法，比如 `++` 、指针计算、string、函数指针/指针函数等，我更愿意称它为 better C。

Go 语法简洁，极少有歧义和噪音，而且不会有隐藏功能或者特别绕的概念，理解起来特别直白简单。

Go 提供了官网代码风格，不会出现茴字有几种写法的问题，因为通常只有一种写法。

Go 随处可见的语法一致性，比如：

- for range 对 string/array/slice/map 的遍历语法基本一致，可以看作是这几种类型与 "for range" 的组合
- struct/interface 的组合风格一致，可以看作是 struct/interface 与 "组合模式" 的组合
- 方法、函数的声明方式基本一致
- 多参数和多返回值的描述方式一致
- 多值赋值和函数多返回值的组合使用

这些简洁的语法经过组合，却能提供强大的表现力。

Go 最终构建成一个二进制可执行程序，往服务器上一丢即可运行，省去了各种环境配置，依赖安装的问题。

Go 的编译速度很快，极大的提升了开发效率。

Go 命令提供了编译、测试、依赖管理、风格校验、死锁检测等功能，一条命令即可调用，非常方便使用。

综上所述：Go 并未有革命性的创新和特性，但是很多特点都直击开发者的痛点。上面说的每一点仔细品味，都会感觉 Go 的精巧设计。

当然 Go 也有很多缺点，比如没有泛型，没有枚举，error 的处理不优雅，早期的模块化方案，都令人诟病。

但是 Go 1.11 增加了模块化方案，现在也出了泛型预览版，预计在 1.17 正式加入泛型。

虽然推进的速度略显缓慢，但是我相信 Go 还是会越来越好的。

那么你又是怎么看的呢？
