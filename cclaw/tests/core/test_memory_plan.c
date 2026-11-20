#include "cc/app/cc_memory_plan.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
/**
 * @brief 检查成功状态并消费结果诊断，失败时打印描述。
 * @param[in] rc 按值移交的拥有结果，调用者不再释放其副本。
 * @return code 为 CC_OK 时为一，否则零，无共享状态。
 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; if (!good) fprintf(stderr,"%s\n",rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }
/**
 * @brief 从非空候选数组选出最低重要度并以最早更新时间打破平局。
 * @param[in] r 调用期只读记录借用，至少含 n 项。
 * @param[in] n 大于零的记录数，由计划构建器保证。
 * @param[in] user 未使用上下文借用，可 NULL。
 * @param[out] out 必填索引槽，写入小于 n 的选中下标。
 * @return 无动态诊断的成功结果，不保留输入，无分配。
 */
static cc_result_t select_lowest(const cc_memory_plan_record_t *r,size_t n,void *user,size_t *out)
{
    (void)user; *out=0;
    for (size_t i=1;i<n;i++)
        if (r[i].importance<r[*out].importance ||
            (r[i].importance==r[*out].importance && r[i].updated_at<r[*out].updated_at)) *out=i;
    return cc_result_ok();
}
/**
 * @brief 构造指向夹具文本的只读记录视图。
 * @param[in] id 调用期标识借用，覆盖计划构建调用。
 * @param[in] text 调用期正文借用，覆盖计划构建调用。
 * @param[in] importance 重要度数值。
 * @param[in] updated 更新时间数值。
 * @return 按值视图，所有字符串为借用，本函数不分配，计划输出由构建器独立复制。
 */
static cc_memory_plan_record_t record(char *id,char *text,unsigned importance,int64_t updated)
{
    cc_memory_plan_record_t r={0}; r.size=sizeof(r); r.id=id; r.text=text; r.category="fact";
    r.evidence="evidence"; r.source_session="session"; r.importance=importance; r.updated_at=updated;
    return r;
}
/**
 * @brief 验证更新、去重、淘汰、元数据变化及缺少选择器和删除缺失项的失败输出。
 * @return 全部断言成功零，失败一，正常路径逐次清理拥有的计划，输入视图保持原样。
 * @note 夹具串行使用栈数组，选择回调不保留借用，无跨线程状态。
 */
int main(void)
{
    cc_memory_plan_record_t existing[]={record("a","A",80,1),record("b","B",20,2),record("c","C",20,3)};
    cc_memory_plan_change_t changes[3]={0};
    changes[0].kind=CC_MEMORY_CHANGE_UPDATE; changes[0].record=record("a","A updated",90,5);
    changes[1].kind=CC_MEMORY_CHANGE_ADD; changes[1].record=record("d","D",30,6);
    changes[2].kind=CC_MEMORY_CHANGE_ADD; changes[2].record=record("duplicate","D",70,7);
    cc_memory_plan_t plan={0};
    CHECK(ok(cc_memory_plan_build(existing,3,changes,3,3,select_lowest,NULL,&plan)));
    CHECK(plan.count==3 && plan.changed_existing==2 && plan.rewritten_existing==2 &&
          plan.added==1 && plan.evicted_existing==1 && plan.evicted_new==0);
    CHECK(strcmp(plan.records[0].id,"a")==0 && strcmp(plan.records[0].text,"A updated")==0);
    CHECK(strcmp(plan.records[1].id,"c")==0 && strcmp(plan.records[2].id,"d")==0);
    CHECK(strcmp(existing[0].text,"A")==0 && existing[1].importance==20);
    cc_memory_plan_cleanup(&plan);
    changes[0].kind=CC_MEMORY_CHANGE_ADD; changes[0].record=record("new-low","new",1,9);
    CHECK(ok(cc_memory_plan_build(existing,3,changes,1,3,select_lowest,NULL,&plan)));
    CHECK(plan.count==3 && plan.changed_existing==0 && plan.rewritten_existing==0 &&
          plan.added==0 && plan.evicted_new==1);
    cc_memory_plan_cleanup(&plan);
    changes[0].kind=CC_MEMORY_CHANGE_UPDATE; changes[0].record=record("a","A",90,9);
    CHECK(ok(cc_memory_plan_build(existing,3,changes,1,3,select_lowest,NULL,&plan)));
    CHECK(plan.changed_existing==1 && plan.rewritten_existing==0);
    cc_memory_plan_cleanup(&plan);
    changes[0].kind=CC_MEMORY_CHANGE_ADD; changes[0].record=record("new-low","new",1,9);
    cc_result_t rc=cc_memory_plan_build(existing,3,changes,1,3,NULL,NULL,&plan);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && !plan.records && plan.count==0); cc_result_free(&rc);
    changes[0].kind=CC_MEMORY_CHANGE_DELETE; changes[0].record=record("missing","ignored",1,0);
    rc=cc_memory_plan_build(existing,3,changes,1,3,select_lowest,NULL,&plan);
    CHECK(rc.code==CC_ERR_NOT_FOUND && !plan.records); cc_result_free(&rc);
    return 0;
}
