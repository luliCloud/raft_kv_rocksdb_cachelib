#include <stdio.h>
#include <glib.h>  // sudo apt-get install libglib2.0-dev (for include this). have underline here, but running good?
#include <stdint.h>
#include <raft-kv/common/log.h>
#include <raft-kv/server/raft_node.h>

static uint64_t g_id = 0;
static const char* g_cluster = NULL;
static uint16_t g_port = 0;

/**
   * Lu: GOptionEntry 结构体，它是 GLib 库中用于命令行参数解析的工具
   * typedef struct {
    const gchar *long_name; -> 'id' 选项的变量名
    gchar        short_name; -> 'i' 选项的短变量名
    gint         flags; -> 0 - 标志位，这里没有指定任何标志。
    GOptionArg   arg; -> G_OPTION_ARG_INT64  选项的参数类型，这里表示参数是一个 64 位整数。
    gpointer     arg_data; -> &g_id 存储选项参数值的变量的地址。注意这里已经说明了这个entry的值存储到&g_id
    const gchar *description; -> "node id"
    const gchar *arg_description; -> NULL
    } GOptionEntry;
*/
int main(int argc, char* argv[]) {
  GOptionEntry entries[] = {
      {"id", 'i', 0, G_OPTION_ARG_INT64, &g_id, "node id", NULL},
      {"cluster", 'c', 0, G_OPTION_ARG_STRING, &g_cluster, "comma separated cluster peers", NULL},
      {"port", 'p', 0, G_OPTION_ARG_INT, &g_port, "key-value server port", NULL},
      {NULL}
  };

  GError* error = NULL;
  //  GOptionContext* context = g_option_context_new("usage");
  //  创建一个新的 GOptionContext 对象，用于解析命令行参数。
  // "usage" 是显示在帮助信息中的一个字符串，通常是描述该程序如何使用的简短说明。
  GOptionContext* context = g_option_context_new("usage");

  // g_option_context_add_main_entries(context, entries, NULL);
  // 将主要的命令行参数（entries 数组）添加到 GOptionContext 对象中。
  g_option_context_add_main_entries(context, entries, NULL);

  // 解析命令行参数，并将结果存储在 g_id、g_port 和 g_cluster 等全局变量中。它们在之前的entry中定义过。所以是一一读到该文件的
  // 相应同名的变量中去。
  // &argc 和 &argv 是标准的命令行参数。使用引用是方便parse这个函数对这些参数进行修改和处理，尤其是错误（为什么要修改？）
  // &error 是一个 GError* 指针，用于存储可能发生的错误信息。
  if (!g_option_context_parse(context, &argc, &argv, &error)) {
    fprintf(stderr, "option parsing failed: %s\n", error->message);
    exit(EXIT_FAILURE);
  }
  fprintf(stderr, "id:%lu, port:%d, cluster:%s\n", g_id, g_port, g_cluster); // 即使成功读取，也会将信息达到err log里

  if (g_id == 0 || g_port == 0) { // 说明g_id 或者 g_port 不存在？ 调用help函数print帮助信息。
    char* help = g_option_context_get_help(context, true, NULL);
    fprintf(stderr, help);
    free(help);
    exit(EXIT_FAILURE);
  }

  kv::RaftNode::main(g_id, g_cluster, g_port);  // kv is the namespace. 
  g_option_context_free(context);
}

/*
注意
在项目内部的函数实现里，并没有依赖Glib，所以在这里Glib主要是对命令行参数进行解析。
在某些项目中，Glib 可能仅被用于特定的功能（如命令行参数解析）。在这种情况下，没必要在整个项目中都依赖 Glib 提供的类型，可以只在必要的地方使用 Glib，而在其他地方使用标准类型。
 * 1. 跨平台兼容性
GLib 提供的数据类型具有跨平台兼容性，这意味着它们在不同操作系统和架构上的表现是一致的。
这在开发需要在多个平台上运行的应用程序时尤为重要。GLib 的数据类型确保了在不同平台上的数据大小和行为一致。
gint64 确保在所有平台上都是 64 位整数。
gchar 确保在所有平台上都是字符类型。

为什么需要先使用 GOptionEntry 接受输入
GOptionEntry 提供了一个结构化的方法来定义和描述命令行选项，使得 GOptionContext 可以根据这些定义来正确解析命令行参数。这样做的好处包括：
结构化和可读性：通过 GOptionEntry 数组可以清晰地定义和描述所有命令行选项，使代码更具可读性和可维护性。
自动生成帮助信息：GLib 可以基于 GOptionEntry 数组自动生成帮助信息。
集中管理：所有命令行选项的定义集中在一个地方，方便管理和修改。

如何通过选项运行这个函数？
./example --id=42 --cluster="peer1,peer2,peer3" --port=8080
或者
./example -i 42 -c "peer1,peer2,peer3" -p 8080
 */
