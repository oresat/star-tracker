
#include <linux/init.h>           // Macros used to mark up functions e.g. __init __exit
#include <linux/module.h>         // Core header for loading LKMs into the kernel
#include <linux/device.h>         // Header to support the kernel Driver Model
#include <linux/kernel.h>         // Contains types, macros, functions for the kernel
#include <linux/fs.h>             // Header for the Linux file system support
#include <linux/uaccess.h>          // Required for the copy to user function
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#define  DEVICE_NAME "ebbchar"    ///< The device will appear at /dev/ebbchar using this value
#define  CLASS_NAME  "ebb"        ///< The device class -- this is a character device driver
#define SIZE 0x200000 //2 MiB is enough for one image

MODULE_LICENSE("GPL");            ///< The license type -- this affects available functionality
MODULE_AUTHOR("Derek Molloy");    ///< The author -- visible when you use modinfo
MODULE_DESCRIPTION("A simple Linux char driver for the BBB");  ///< The description -- see modinfo
MODULE_VERSION("0.1");            ///< A version number to inform users

static int    majorNumber;                  ///< Stores the device number -- determined automatically
static char   message[256] = {0};           ///< Memory for the string that is passed from userspace
static short  size_of_message;              ///< Used to remember the size of the string stored
static int    numberOpens = 0;              ///< Counts the number of times the device is opened
static struct class*  ebbcharClass  = NULL; ///< The device-driver class struct pointer
static struct device* ebbcharDevice = NULL; ///< The device-driver device struct pointer

// The prototype functions for the character driver -- must come before the struct definition
static int     dev_open(struct inode *, struct file *);
static int     dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static int pru_handshake(int physAddr);
static irq_handler_t irqhandler(unsigned int irq, void *dev_id, struct pt_regs *regs);
int pru_probe(struct platform_device*);
int pru_rm(struct platform_device*);
int release_dev(void);
int irq = 62;

/** @brief Devices are represented as file structure in the kernel. The file_operations structure from
 *  /linux/fs.h lists the callback functions that you wish to associated with your file operations
 *  using a C99 syntax structure. char devices usually implement open, read, write and release calls
 */
static struct file_operations fops =
{
  .open = dev_open,
  .read = dev_read,
  .write = dev_write,
  .release = dev_release,
};

//Memory pointers: TODO is there anything wrong with these being global?
dma_addr_t dma_handle = NULL;
int *cpu_addr = NULL;

static struct resource prudev_resources[] = {
  {
    .start = 20,
    .end = 20,
    .flags = IORESOURCE_IRQ,
    .name = "irq"
  }
};

struct platform_device *prudev;

static const struct of_device_id my_of_ids[] = {
  { .compatible = "prudev,prudev" },
  { },
};

static struct platform_driver prudrvr = {
  .driver = {
    .name = "prudev",
    .owner = THIS_MODULE,
    .of_match_table = my_of_ids
  },
  .probe = pru_probe,
  .remove = pru_rm,
};


int pru_probe(struct platform_device* dev)
{
  printk(KERN_INFO "EBBChar: probing %s\n", dev->name);

  
  //   for(int i = 0 ; i < 128 ; i++) {
  //TODO THIS IS STILL RETURNING -6 NO DEVICE
  //int irq_num = platform_get_irq(dev, i);
  //int num = 24;
  //int irq_num = platform_get_irq(dev, num);
  int irq_num = platform_get_irq_byname(dev, "20");
  printk(KERN_INFO "EBBChar: platform_get_irq(%d) returned: %d\n", 1, irq_num);
  irq = request_irq(irq_num, (irq_handler_t)irqhandler, 0, "prudev", NULL);

  //}

  //  int pru_evt0_irq = 62;
  //  irq = request_irq(pru_evt0_irq, (irq_handler_t)irqhandler, 0, "prudev", NULL);
  //  printk(KERN_ERR "EBBChar: platform_get_irq(%d) returned: %d\n", pru_evt0_irq, irq);


  return 0;
}

int release_dev(void) {
  return 0;
}

int pru_rm(struct platform_device* dev)
{
  return 0;
}

/** @brief The LKM initialization function
 *  The static keyword restricts the visibility of the function to within this C file. The __init
 *  macro means that for a built-in driver (not a LKM) the function is only used at initialization
 *  time and that it can be discarded and its memory freed up after that point.
 *  @return returns 0 if successful
 */

static int __init ebbchar_init(void){
  printk(KERN_INFO "EBBChar: Initializing the EBBChar v1\n");

  // Try to dynamically allocate a major number for the device -- more difficult but worth it
  majorNumber = register_chrdev(0, DEVICE_NAME, &fops);
  if (majorNumber<0){
    printk(KERN_ALERT "EBBChar failed to register a major number\n");
    return majorNumber;
  }
  printk(KERN_INFO "EBBChar: registered correctly with major number %d\n", majorNumber);

  // Register the device class
  ebbcharClass = class_create(THIS_MODULE, CLASS_NAME);
  if (IS_ERR(ebbcharClass)){                // Check for error and clean up if there is
    unregister_chrdev(majorNumber, DEVICE_NAME);
    printk(KERN_ALERT "Failed to register device class\n");
    return PTR_ERR(ebbcharClass);          // Correct way to return an error on a pointer
  }
  printk(KERN_INFO "EBBChar: device class registered correctly\n");

  int regDrvr = platform_driver_register(&prudrvr);
  printk(KERN_INFO "EBBChar: platform driver register returned: %d\n", regDrvr);

  //printk(KERN_INFO "EBBChar: platform device registered\n");

  // Register the device driver
  ebbcharDevice = device_create(ebbcharClass, NULL, MKDEV(majorNumber, 0), NULL, DEVICE_NAME);
  if (IS_ERR(ebbcharDevice)){               // Clean up if there is an error
    class_destroy(ebbcharClass);           // Repeated code but the alternative is goto statements
    unregister_chrdev(majorNumber, DEVICE_NAME);
    printk(KERN_ALERT "Failed to create the device\n");
    return PTR_ERR(ebbcharDevice);
  }
  printk(KERN_INFO "EBBChar: device class created correctly\n"); // Made it! device was initialized
  return 0;
}


/** @brief The LKM cleanup function
 *  Similar to the initialization function, it is static. The __exit macro notifies that if this
 *  code is used for a built-in driver (not a LKM) that this function is not required.
 */

static void __exit ebbchar_exit(void){
  //unregister platform device and driver
  platform_device_unregister(prudev);
  platform_driver_unregister(&prudrvr);

  device_destroy(ebbcharClass, MKDEV(majorNumber, 0));     // remove the device
  class_unregister(ebbcharClass);                          // unregister the device class
  class_destroy(ebbcharClass);                             // remove the device class
  //free_irq(62, NULL);
  unregister_chrdev(majorNumber, DEVICE_NAME);             // unregister the major number
  printk(KERN_INFO "EBBChar: Goodbye from the LKM!\n");
}


static int dev_open(struct inode *inodep, struct file *filep){
  int ret = 0; //return value
  //set DMA mask
  int retMask = dma_set_coherent_mask(ebbcharDevice, 0xffffffff);
  if(retMask != 0)
  {
    printk(KERN_INFO "Failed to set DMA mask : error %d\n", retMask);
    return 0;
  }

  // int regDev = platform_device_register(&prudev);
  //using the "simple" function stopped the "no release func" error
  prudev = platform_device_register_simple("prudev", 1, prudev_resources, 1);
  //TODO how do I check for success here


  /*
   * I initial used GFP_DMA flag below, but I could not allocate >1 MiB
   * I am unsure what the ideal flags for this are, but GFP_KERNEL seems to
   * work
   */
  cpu_addr = dma_alloc_coherent(ebbcharDevice, SIZE, &dma_handle, GFP_KERNEL);
  if(cpu_addr == NULL)
  {
    printk(KERN_INFO "Failed to allocate memory\n");
    return 0;
  }

  printk(KERN_INFO "Virtual Address: %x\n", (int)cpu_addr);
  printk(KERN_INFO "Physical Address: %x\n", (int)dma_handle);

  int* physAddr = (int*)dma_handle;
  printk(KERN_INFO "physAddr: %x\n", (int)physAddr);

  /*
  //irq = 62;
  for( irq = 0 ; irq < 256 ; irq++)
  {
  int pru_evt0_irq = irq;
  int irqRet = request_irq(pru_evt0_irq, (irq_handler_t)irqhandler, 0, "prudev", NULL);
  printk(KERN_ERR "EBBChar: request_irq(%d) returned: %d\n", pru_evt0_irq, irqRet);
  }
  */

  /* SKIP THE HANDSHAKE FOR NOW WHILE WE'RE TESTING INTERRUPTS
     int handshake = pru_handshake((int)physAddr);
     if(handshake < 0) 
     {
     ret = -1;
     printk(KERN_ERR "PRU Handshake failed: %x\n", (int)physAddr);
     goto exit;
     }
     */

exit:
  return ret;
}

//TODO do I need to check for some sort of error on readl() and writel()

/* 
 * Below is a basic handshake with the PRU. This module writes the value of
 * physical memory pointer to a mutually agreed upon(between kernel and PRU)
 * memory location in the PRU shared RAM. The PRU reads this value,
 * increments it by a agreed upon key, and writes it back to the memory
 * location + 1. The kernel driver sees this value, increments it again and
 * writes it back that the memory location + 2. The PRU sees this value,
 * verifying the handshake and moving on.
 * I am writing without permission to the PRU shared RAM. This
 * should be ok, but in the future I should set aside of piece of PRU shared
 * RAM to ensure it doesn't accidentally use i
 */
static int pru_handshake(int physAddr )
{
  //TODO move these above or to header
#define PRUBASE 0x4a300000
#define PRUSHAREDRAM PRUBASE + 0x10000
#define PRUWRBACK PRUSHAREDRAM + 0x4
#define KERNELWRBACK PRUSHAREDRAM + 0x8
#define WRITEBACK_KEY 0x1234
  printk(KERN_INFO "pru shared RAM: %x\n", PRUSHAREDRAM);
  printk(KERN_INFO "pru write back: %x\n", PRUWRBACK);

  //ioremap physical locations in the PRU shared ram 
  void __iomem *pru_shared_ram;
  pru_shared_ram = ioremap_nocache((int)PRUSHAREDRAM, 16);
  printk(KERN_INFO "pru_shared_ram virt: %x\n", (int)pru_shared_ram);

  //where the PRU will write back the key to kernel
  void __iomem *pru_write_back = pru_shared_ram + 4;
  printk(KERN_INFO "pru_write_back virt: %x\n", (int)pru_write_back);

  //where the kernel will re-write back to PRU
  void __iomem *kernel_write_back = pru_shared_ram + 8;
  printk(KERN_INFO "kernel_write_back virt: %x\n", (int)kernel_write_back);

  //zero PRU writeback data
  writel(0x0, pru_write_back);

  //write physical address to PRU shared RAM where a PRU can find it
  writel(physAddr, pru_shared_ram);

  int key = (int)physAddr + WRITEBACK_KEY;
  printk(KERN_INFO "key: %x\n", key);

  volatile int i = 0;
  int success = 0;

  int pru_read_back;

  //wait for a short time for the PRU to respond to the handshake
  for(i ; i < 1<<8 ; i++)
  {
    pru_read_back = readl(pru_write_back);
    if(pru_read_back !=  key)
      continue;
    success = 1;
    break;
  }

  int ret = 0;
  if(!success)
  {
    printk(KERN_INFO "writeback fail!\n");
    ret = -1;
    goto handshake_exit;
  }else{
    printk(KERN_INFO "PRU replied successfully!\n");
  }

  printk(KERN_INFO "Writing back to PRU !\n");
  key += WRITEBACK_KEY;

  writel(key, kernel_write_back);

handshake_exit:
  //unmap iomem
  iounmap(pru_shared_ram);

  return ret;
}

/** @brief This function is called whenever device is being read from user space i.e. data is
 *  being sent from the device to the user. In this case is uses the copy_to_user() function to
 *  send the buffer string to the user and captures any errors.
 *  @param filep A pointer to a file object (defined in linux/fs.h)
 *  @param buffer The pointer to the buffer to which this function writes the data
 *  @param len The length of the b
 *  @param offset The offset if required
 */
static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset){
  int error_count = 0;
  // copy_to_user has the format ( * to, *from, size) and returns 0 on success
  error_count = copy_to_user(buffer, message, size_of_message);

  if (error_count==0){            // if true then have success
    printk(KERN_INFO "EBBChar: Sent %d characters to the user\n", size_of_message);
    return (size_of_message=0);  // clear the position to the start and return 0
  }
  else {
    printk(KERN_INFO "EBBChar: Failed to send %d characters to the user\n", error_count);
    return -EFAULT;              // Failed -- return a bad address message (i.e. -14)
  }
}

/** @brief This function is called whenever the device is being written to from user space i.e.
 *  data is sent to the device from the user. The data is copied to the message[] array in this
 *  LKM using the sprintf() function along with the length of the string.
 *  @param filep A pointer to a file object
 *  @param buffer The buffer to that contains the string to write to the device
 *  @param len The length of the array of data that is being passed in the const char buffer
 *  @param offset The offset if required
 */
static ssize_t dev_write(struct file *filep, const char *buffer, size_t len, loff_t *offset){
  printk(KERN_INFO "============WRITE============\n");
  copy_from_user(message,buffer,5);
  size_of_message = strlen(message);                 // store the length of the stored message
  printk(KERN_INFO "len: %d\n", size_of_message);
  //printk(KERN_INFO "buffer: %c\n", buffer[0]);
  printk(KERN_INFO "message: %c\n", message[0]);
  //message[0] = 'g';
  printk(KERN_INFO "============WRITE============\n");
  printk(KERN_INFO "EBBChar: Received %zu characters from the user\n", len);
  return len;
}

static irq_handler_t irqhandler(unsigned int irq, void *dev_id, struct pt_regs *regs)
{
  printk(KERN_INFO "IRQ_HANDLER!!!\n");
  return (irq_handler_t) 0;
}

/** @brief The device release function that is called whenever the device is closed/released by
 *  the userspace program
 *  @param inodep A pointer to an inode object (defined in linux/fs.h)
 *  @param filep A pointer to a file object (defined in linux/fs.h)
 */
static int dev_release(struct inode *inodep, struct file *filep){
  dma_free_coherent(ebbcharDevice, SIZE, cpu_addr, dma_handle);
  printk(KERN_INFO "Freed %d bytes\n", SIZE); 

  printk(KERN_INFO "EBBChar: Device successfully closed\n");
  return 0;
}

/** @brief A module must use the module_init() module_exit() macros from linux/init.h, which
 *  identify the initialization function at insertion time and the cleanup function (as
 *  listed above)
 */
module_init(ebbchar_init);
module_exit(ebbchar_exit);